#include "vm/page.h" 
#include <debug.h>
#include <round.h>
#include <stdlib.h>

#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "filesys/file.h"
#include "filesys/inode.h"
#include "filesys/filesys.h"

#include "vm/frame.h"
#include "vm/swap.h"

/* Maximum distance from stack we can be in order to count as
   stack growth rather than an invalid memory access. */
#define MAX_VALID_STACK_DISTANCE 31

/* System-wide table of read-only shared segments.
   Processes can share entries in this table instead of maintaining
   their own copy of all of the pages they use. */
struct ro_shared_mappings_table ro_shared_mappings_table;

/* Private function declarations. */
/* TODO for X_init() functions that are really X_create() functions, change the names. 
   I think that might be all of these. */

/* Supp page table. */
static bool supp_page_table_is_range_valid (struct supp_page_table *, void *, void *);
static bool supp_page_table_is_stack_growth (struct supp_page_table *, void *);

/* Segment. */
static struct segment * segment_init (void *, void *, struct file *, int, enum segment_type); 
static void segment_destroy (struct segment *);

static struct page * segment_get_page (struct segment *, int32_t);
static int32_t segment_calc_page_num (struct segment *, void *);
static void * segment_calc_vaddr (struct segment *, int32_t);
static bool segment_list_less_func (const struct list_elem *, const struct list_elem *, void *);

/* Page. */
static struct page * page_init (struct segment_mapping_info *, int32_t);
static void page_destroy (struct page *);
static void page_set_hash (struct page *, unsigned);
static unsigned page_hash_func (const struct hash_elem *, void *);
static bool page_hash_less_func (const struct hash_elem *, const struct hash_elem *, void *);
static bool page_remove_owner (struct page *, struct segment *);

/* Shared mappings. */
static void shared_mappings_init (struct shared_mappings *, struct file *, int);
static void shared_mappings_destroy (struct shared_mappings *);
static void shared_mappings_destroy_hash_func (struct hash_elem *, void *);
static void shared_mappings_incr_ref_count (struct shared_mappings *);
static void shared_mappings_decr_ref_count (struct shared_mappings *);
static void shared_mappings_set_hash (struct shared_mappings *, unsigned);
static unsigned shared_mappings_hash_func (const struct hash_elem *, void *);
static bool shared_mappings_hash_less_func (const struct hash_elem *, const struct hash_elem *, void *);

/* ro_shared_mappings_table. */
static struct shared_mappings *ro_shared_mappings_table_add (struct file *, int); 

/* page_owner_info. */
static bool page_owner_info_list_less_func (const struct list_elem *, const struct list_elem *, void *);

/* Initialize the ro shared segment table. Not thread safe. Should be called once. */
void 
ro_shared_mappings_table_init (void)
{
  hash_init (&ro_shared_mappings_table.inumber_to_segment, shared_mappings_hash_func, shared_mappings_hash_less_func, NULL);
  lock_init (&ro_shared_mappings_table.hash_lock);
}

/* Destroy the ro shared segment table. Not thread safe. Should be called once. */
void 
ro_shared_mappings_table_destroy (void)
{
  hash_destroy (&ro_shared_mappings_table.inumber_to_segment, shared_mappings_destroy_hash_func);
}

/* Get the shared_mappings associated with INODE.
   If no such segment yet exists, one is created. */
struct shared_mappings * 
ro_shared_mappings_table_get (struct file *f, int flags)
{
  ASSERT (f != NULL);

  struct shared_mappings *match = NULL;

  struct shared_mappings dummy;
  block_sector_t inumber = inode_get_inumber (file_get_inode (f));
  shared_mappings_set_hash (&dummy, inumber);

  lock_acquire (&ro_shared_mappings_table.hash_lock);

  struct hash_elem *e = hash_find (&ro_shared_mappings_table.inumber_to_segment, &dummy.elem);
  if (e)
    match = hash_entry (e, struct shared_mappings, elem);
  else
    match = ro_shared_mappings_table_add (f, flags);

  /* Increment ref count while we hold hash_lock to avoid race with ro_shared_mappings_table_remove. */
  shared_mappings_incr_ref_count (match);

  lock_release (&ro_shared_mappings_table.hash_lock);

  return match;
}

/* Add a new shared_mappings to ro_shared_mappings_table.
   Caller must have locked ro_shared_mappings_table. 
   Returns the shared segment we add. It has ref_count 0. 

   F must be a "private" file*: a dup of whatever the original
     file was. */
struct shared_mappings *
ro_shared_mappings_table_add (struct file *f, int flags)
{
  ASSERT (f != NULL);

  struct shared_mappings *new_sm = (struct shared_mappings *) malloc (sizeof(struct shared_mappings));
  ASSERT (new_sm != NULL);

  shared_mappings_init (new_sm, f, flags);
  hash_insert (&ro_shared_mappings_table.inumber_to_segment, &new_sm->elem);

  return new_sm;
}

/* page_owner_info functions. */

/* Return true if less, false else. 
   If there's a tie on tid, use vaddr. */
static bool page_owner_info_list_less_func (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
  ASSERT (a != NULL);
  ASSERT (b != NULL);

  struct page_owner_info *a_info = list_entry (a, struct page_owner_info, elem);
  struct page_owner_info *b_info = list_entry (b, struct page_owner_info, elem);

  bool is_same_tid = (a_info->owner->tid == b_info->owner->tid);
  if (is_same_tid)
    /* Tie goes to the vpg_addr. This allows a single process to have multiple shared
       memory mappings to the same underlying place (e.g. two shared mappings of the same
       file). */
    return a_info->vpg_addr < b_info->vpg_addr;
  else
    return a_info->owner->tid < b_info->owner->tid;
}

/* Remove the (unlocked) shared_mappings associated with F.
   (unless there is a reference to it). 

   Beware of race conditions; see comments inside. */
void 
ro_shared_mappings_table_remove (struct file *f)
{
  ASSERT (f != NULL);
  struct shared_mappings *match = NULL;

  block_sector_t inumber = inode_get_inumber (file_get_inode (f));
  struct shared_mappings dummy;
  shared_mappings_set_hash (&dummy, inumber);

  lock_acquire (&ro_shared_mappings_table.hash_lock);

  struct hash_elem *e = hash_find (&ro_shared_mappings_table.inumber_to_segment, &dummy.elem);
  /* If another process has established a mapping between when we were
     called and now, and has finished and called this function, then
     e may be NULL. */
  if (e)
  {
    match = hash_entry (e, struct shared_mappings, elem);
    /* If another process has established a mapping between when we were 
       called and now, then the shared_mappings may have a non-zero ref count. */
    if (match->ref_count == 0)
      shared_mappings_destroy (match);
  }

  lock_release (&ro_shared_mappings_table.hash_lock);
}

/* Initialize a supplemental page table.
   This SPT begins with only a stack segment. */
void 
supp_page_table_init (struct supp_page_table *spt)
{
  ASSERT (spt != NULL);

  list_init (&spt->segment_list);

  /* Create a stack segment. */
  struct segment *stack_seg = segment_init (PHYS_BASE - PGSIZE, PHYS_BASE, NULL, 0, SEGMENT_PRIVATE);
  list_insert_ordered (&spt->segment_list, &stack_seg->elem, segment_list_less_func, NULL);
}

/* Destroy this supplemental page table, releasing
     all resources it holds. 
   Caller is responsible for freeing the memory
     associated with the spt, if dynamic. */
void 
supp_page_table_destroy (struct supp_page_table *spt)
{
  ASSERT (spt != NULL);

  while (!list_empty (&spt->segment_list))
  {
    struct list_elem *e = list_pop_front (&spt->segment_list);
    ASSERT (e != NULL);
    segment_destroy (list_entry (e, struct segment, elem));
  }
}

/* Return the page associated with virtual address VADDR.
   If VADDR looks like a stack growth, grow the stack and add a page.

   Returns NULL if no such page (i.e. illegal memory access). */
struct page * 
supp_page_table_find_page (struct supp_page_table *spt, void *vaddr)
{
  ASSERT (spt != NULL);
  ASSERT (vaddr != NULL);

  struct segment *seg = NULL;
  struct page *ret = NULL;

  /* Find the segment to which this address belongs. */
  struct segment *tmp_seg = NULL;
  struct list_elem *e = NULL;
  size_t list_size = 0;
  for (e = list_begin (&spt->segment_list); e != list_end (&spt->segment_list);
       e = list_next (e))
  {
    list_size++;
    tmp_seg = list_entry (e, struct segment, elem);
    if (tmp_seg->start <= vaddr && vaddr < tmp_seg->end)
    {
      seg = tmp_seg;
      break;
    }
  }

  /* Found a matching segment. */
  if (seg)
  {
    /* mappings is keyed by page number. */
    int32_t page_num = segment_calc_page_num (seg, vaddr);
    return segment_get_page (seg, page_num);
  }
  /* Did not find a matching segment. Could still be stack growth. */
  else
  {
    bool is_stack_growth = supp_page_table_is_stack_growth (spt, vaddr);
    /* Stack growth? Need to add a page. 
       We know we aren't overlapping our predecessor because if we were, we would
         not have page faulted. Behavior in such a case is undefined. */
    if (is_stack_growth)
    {
      seg->start -= PGSIZE;
      /* mappings is keyed by page number. */
      int32_t page_num = segment_calc_page_num (seg, vaddr);
      ret = segment_get_page (seg, page_num);
      ASSERT (ret != NULL);
    }
  }

  return ret;
}

/* Add a memory mapping to supp page table SPT
     for file F beginning at START with flags FLAGS.
   Returns NULL on failure.

   F must be a "private" file*: a dup of whatever the original
     file was.

   Returns the new segment on success.
   Returns NULL if range is not valid or on failure.

   Use supp_page_table_remove_segment() to clean up the segment. */
struct segment * 
supp_page_table_add_mapping (struct supp_page_table *spt, struct file *f, void *start, int flags, bool is_shared)
{
  ASSERT (spt != NULL);
  ASSERT (f != NULL);

  void *end = (void *) ((uint32_t) start + file_length (f));
  if (!supp_page_table_is_range_valid (spt, start, end))
    return NULL;

  struct segment *ret = segment_init (start, end, f, flags, is_shared ? SEGMENT_SHARED : SEGMENT_PRIVATE);
  list_insert_ordered (&spt->segment_list, &ret->elem, segment_list_less_func, NULL);
  return ret;
}

/* Remove the specified segment from this page table.
   If it's an mmap'd segment, will flush all dirty pages. 
   Will free the memory associated with this segment
   If it's a shared mapping, and if we're the last holder of the 
     pages, will free that memory too. */
void 
supp_page_table_remove_segment (struct supp_page_table *spt, struct segment *seg)
{
  ASSERT (spt != NULL);
  ASSERT (seg != NULL);

  list_remove (&seg->elem);
  segment_destroy (seg);
}

/* Determine whether or not this range is valid:
    - must start above 0
    - must be in user-space (below PHYS_BASE)
    - must be page-aligned 
    - must not overlap with any existing segments in SPT
    
   Returns true if valid, false else. */
bool 
supp_page_table_is_range_valid (struct supp_page_table *spt, void *start, void *end)
{
  ASSERT (spt != NULL);

  uint32_t start_addr = (uint32_t) start;
  uint32_t end_addr = (uint32_t) end;

  /* start must precede end. */
  ASSERT (start < end);

  /* - must start above 0 */
  if (start_addr == 0)
    return false;
  /* - must be in user-space (below PHYS_BASE) */
  if (PHYS_BASE < end)
    return false;
  /* - must be page-aligned */
  if (start_addr % PGSIZE != 0)
    return false;

  /* - must not overlap with any existing segments in SPT */
  struct segment *seg = NULL;
  struct list_elem *e;
  for (e = list_begin (&spt->segment_list); e != list_end (&spt->segment_list);
       e = list_next (e))
  {
    seg = list_entry (e, struct segment, elem);
    ASSERT (seg != NULL);
    /* We don't overlap if: segment ends before we begin, or we end before segment begins. */
    bool does_not_overlap = ((uint32_t) seg->end < start_addr || end_addr < (uint32_t) seg->start);
    if (does_not_overlap)
      continue;
    else
      return false;
  }

  return true;
}

/* Return true if this address is stack growth, false else. */
bool 
supp_page_table_is_stack_growth (struct supp_page_table *spt, void *vaddr)
{
  ASSERT (spt != NULL);

  /* SPT's list of segments is sorted, so stack is the final element. */
  struct segment *seg = list_entry (list_back (&spt->segment_list), struct segment, elem);
  ASSERT (seg != NULL);
  /* Use casting to make sure we have a valid number for abs. */
  int64_t distance_from_stack = (uint64_t) seg->start - (uint64_t) vaddr;
  if (llabs (distance_from_stack) <= MAX_VALID_STACK_DISTANCE)
    return true;
  return false;
}

/* Initialize a segment. Destroy with segment_destroy(). */
struct segment * 
segment_init (void *start, void *end, struct file *mmap_file, int flags, enum segment_type type)
{
  struct segment *seg = (struct segment *) malloc (sizeof(struct segment));
  ASSERT (seg != NULL);

  ASSERT ((uint32_t) start < (uint32_t) end);

  seg->start = start;
  seg->end = end;
  seg->type = type;

  if (seg->type == SEGMENT_PRIVATE)
  {
    /* Initialize and set mappings to a struct hash*. */
    seg->mappings = malloc (sizeof(struct segment_mapping_info));
    ASSERT (seg->mappings != NULL);

    struct segment_mapping_info *smi = (struct segment_mapping_info *) seg->mappings;
    hash_init (&smi->mappings, page_hash_func, page_hash_less_func, NULL);
    smi->mmap_file = mmap_file;
    smi->flags = flags;
  }
  else if (seg->type == SEGMENT_SHARED)
  {
    /* We only support shared segments for mmap'd files. */
    ASSERT (mmap_file != NULL);
    /* Set mappings to the appropriate struct shared_mappings*. */
    seg->mappings = (void *) ro_shared_mappings_table_get (mmap_file, flags);
  }
  else
    NOT_REACHED ();

  return seg;
}

/* Destroy segment SEG created by segment_init(). 
   Acquires and releases filesys_lock. */
void 
segment_destroy (struct segment *seg)
{
  ASSERT (seg != NULL);

  struct segment_mapping_info *smi = NULL;
  struct shared_mappings *sm = NULL;

  if (seg->type == SEGMENT_PRIVATE)
    smi = (struct segment_mapping_info *) seg->mappings;
  else
  {
    sm = (struct shared_mappings *) seg->mappings;
    smi = &sm->smi;
  }

  /* Iterate over the mappings, removing ourselves from the list of owners for each. 
     We must be the sole modifier of the hash at this time, hence the lock in the case
     of a shared mapping. */
  if (sm)
    lock_acquire (&sm->segment_mapping_info_lock);

  struct hash *h = &smi->mappings;
  struct hash_iterator hi;
  hash_first (&hi, h);
  struct hash_elem *he = hash_cur (&hi);
  struct hash_elem *next = NULL;
  while (he != NULL)
  {
    next = hash_next (&hi);
    struct page *pg = (struct page *) hash_entry (he, struct page, elem);
    lock_acquire (&pg->lock);
    bool was_sole_owner = page_remove_owner (pg, seg);
    if (seg->type == SEGMENT_PRIVATE)
    {
      ASSERT (was_sole_owner);
    }
    if (was_sole_owner)
    {
      hash_delete (h, &pg->elem);
      page_destroy (pg);
    }
    else
    {
      lock_release (&pg->lock);
    }
    he = next;
  }

  if (sm)
    lock_release (&sm->segment_mapping_info_lock);

  /* Remaining cleanup for private segments. */
  if (seg->type == SEGMENT_PRIVATE)
  {
    /* Private mapping, so hash should be empty now. */
    ASSERT (hash_size (&smi->mappings) == 0);
    /* Destroy the hash. */
    hash_destroy (&smi->mappings, NULL);
    if (smi->mmap_file)
    {
      filesys_lock ();
      file_close (smi->mmap_file);
      filesys_unlock ();
    }
  }
  /* Remaining cleanup for shared segments. */
  else
  {
    /* We are now no longer using these mappings, so decrement ref count. */
    shared_mappings_decr_ref_count ((struct shared_mappings *) seg->mappings);
  }

  free (seg);
}

/* Retrieve the specified page from SEG. 
   Adds a page if no such page has yet been defined. */ 
struct page * 
segment_get_page (struct segment *seg, int32_t relative_page_num)
{
  ASSERT (seg != NULL);
  ASSERT (seg->mappings != NULL);

  int32_t seg_n_pages = ((uint32_t) seg->end - (uint32_t) seg->start) / PGSIZE;
  ASSERT (0 <= relative_page_num && relative_page_num < seg_n_pages);

  struct page *ret = NULL;

  struct page dummy;
  page_set_hash (&dummy, relative_page_num);

  /* Test this to see if we need to lock. */
  struct shared_mappings *sm = NULL;

  /* Get pointer to the smi and the mappings hash. */
  struct segment_mapping_info *smi = NULL;
  struct hash *h = NULL;
  if (seg->type == SEGMENT_PRIVATE)
  {
    smi = (struct segment_mapping_info *) seg->mappings;
    h = &smi->mappings;
  }
  else
  {
    sm = (struct shared_mappings *) seg->mappings;
    smi = &sm->smi;
    h = &sm->smi.mappings;
  }
  ASSERT (smi != NULL);
  ASSERT (h != NULL);

  if (sm)
    lock_acquire (&sm->segment_mapping_info_lock);

  /* Find the page. Add a new one if there isn't one yet. */
  struct hash_elem *e = hash_find (h, &dummy.elem);
  if (e)
    ret = hash_entry (e, struct page, elem);
  else
  {
    /* Add a new page. */
    ret = page_init (smi, relative_page_num);
    hash_insert (h, &ret->elem);
  }

  /* TODO Refactor this into a page_add_owner matching page_remove_owner. */
  /* Now that we've got a page, we need to make sure we are on the list of owners of the page. 
     In the event that it's a shared page, we may or may not already be on the list. 
     If it's a new page, we're definitely not on the list (and the list is empty). 
     
     We hold sm->segment_mapping_info_lock, so the page we found/created is not going to disappear
     out from under us.  */

  /* Lock page so that the owners member is fixed. */
  lock_acquire (&ret->lock);

  struct page_owner_info *poi = (struct page_owner_info *) malloc (sizeof(struct page_owner_info));
  ASSERT (poi != NULL);
  poi->owner = thread_current ();
  poi->vpg_addr = segment_calc_vaddr (seg, relative_page_num);

  /* If insertion fails, a matching poi is already present and we can just free the memory. */
  if (!list_insert_ordered_unique (&ret->owners, &poi->elem, page_owner_info_list_less_func, NULL))
    free (poi);

  lock_release (&ret->lock);

  if (sm)
    lock_release (&sm->segment_mapping_info_lock);

  return ret;
}

/* Calculate the relative page number of VADDR in segment SEG.
   This is the inverse of segment_calc_vaddr. */
int32_t 
segment_calc_page_num (struct segment *seg, void *vaddr)
{
  ASSERT (seg != NULL);
  ASSERT ((uint32_t) seg->start <= (uint32_t) vaddr && (uint32_t) vaddr < (uint32_t) seg->end);

  uint32_t *vpgaddr = ROUND_DOWN ( (uint32_t) vaddr, PGSIZE);

  /* If segment grows up (all segments but stack), then we calculate
       page number based on seg->start. seg->end is fixed.
     If segment grows down (stack segment), then we calculate
       page number based on seg->end. seg->start is fixed.

     NB If we need to grow mmap'd files in P4, make growth direction
       a member of a segment.

     We do not handle segments that can grow in both directions. */
  bool segment_grows_up = (seg->end < PHYS_BASE ? true : false);

  int32_t page_no;
  if (segment_grows_up)
    page_no = ((uint32_t) vpgaddr - (uint32_t) seg->start) / PGSIZE;
  else
    /* 1 <= (end - vpgaddr)/PGSIZE, so to get indexing from 0 we subtract 1. */
    page_no = (((uint32_t) seg->end - (uint32_t) vpgaddr) / PGSIZE) - 1;
  return page_no;
}

/* Calculate the virtual page offset of relative page RELATIVE_PAGE_NUM in segment SEG. 
   This is the inverse of segment_calc_page_num. */
static void * 
segment_calc_vaddr (struct segment *seg, int32_t relative_page_num)
{
  ASSERT (seg != NULL);

  uint32_t max_page_num = (seg->end - seg->start) / PGSIZE;
  ASSERT (relative_page_num < max_page_num);

  /* Just like in segment_calc_page_num: 
       - determine if segment grows up or down
       - calculate based on seg->start or seg->end as appropriate */
  bool segment_grows_up = (seg->end < PHYS_BASE ? true : false);

  uint32_t page_addr;
  if (segment_grows_up)
    page_addr = seg->start + relative_page_num*PGSIZE;
  else
    page_addr = seg->end - (relative_page_num + 1)*PGSIZE;
  return (void *) page_addr;
}

/* Segment list_less_func. */
bool 
segment_list_less_func (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
  ASSERT (a != NULL);
  ASSERT (b != NULL);

  struct segment *a_seg = list_entry (a, struct segment, elem);
  struct segment *b_seg = list_entry (b, struct segment, elem);

  return (uint32_t) a_seg->start < (uint32_t) b_seg->start;
}

/* Page functions. */

/* Initialize a page with no owners.
   Destroy with page_destroy(). */
struct page * 
page_init (struct segment_mapping_info *smi, int32_t segment_page)
{
  ASSERT (smi != NULL);
  ASSERT (0 <= segment_page);

  struct page *pg = (struct page *) malloc (sizeof(struct page));
  ASSERT (pg != NULL);

  list_init (&pg->owners);
  pg->location = NULL;
  pg->stamp = 0;
  pg->status = PAGE_NEVER_ACCESSED;
  pg->smi = smi;
  pg->segment_page = segment_page;
  lock_init (&pg->lock);

  return pg;
}

/* Destroy locked page PG created by page_init(). */
void 
page_destroy (struct page *pg)
{
  ASSERT (pg != NULL);

  /* Only the last owner should be destroying this page. 
     In this case, pg->owners is empty. */
  ASSERT (list_size (&pg->owners) == 0);

  /* TODO I shouldn't know that the swap table exists. Add a bool to struct page for frame's use,
     and just call frame_table_release_page? Or is this needlessly circuitous? */
  if (pg->status == PAGE_RESIDENT)
    frame_table_release_page (pg);
  else if (pg->status == PAGE_SWAPPED_OUT)
    swap_table_discard_page (pg);

  pg->status = PAGE_DISCARDED;

  free (pg);
}

/* Set PG's fields to hash to KEY. */
void 
page_set_hash (struct page *pg, unsigned key)
{
  ASSERT (pg != NULL);
  pg->segment_page = (int32_t) key;
} 

/* Hash this page. */
unsigned 
page_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  ASSERT (e != NULL);
  struct page *pg = hash_entry (e, struct page, elem);
  ASSERT (pg != NULL);
  return pg->segment_page;
}

/* Which page is the lesser? */ 
bool 
page_hash_less_func (const struct hash_elem *a, const struct hash_elem *b, void *aux)
{
  ASSERT (a != NULL);
  ASSERT (b != NULL);
  
  /* KISS. */
  return page_hash_func (a, aux) < page_hash_func (b, aux);
}

/* Remove self from this locked page's list of owners. 
   If we were the last owner, return true. */
static bool 
page_remove_owner (struct page *pg, struct segment *seg)
{
  ASSERT (pg != NULL);
  ASSERT (seg != NULL);

  /* A page_owner_info matching what this process would have put into PG->owners were
     it registered as an owner of PG. */
  struct page_owner_info dummy;
  dummy.owner = thread_current ();
  dummy.vpg_addr = segment_calc_vaddr (seg, pg->segment_page);
  struct list_elem *removed_poi_elem = list_remove_ordered (&pg->owners, &dummy.elem, page_owner_info_list_less_func, NULL);

  /* If we were a registered owner, free the poi we allocated. */
  if (removed_poi_elem != NULL)
  {
    struct page_owner_info *poi = list_entry (removed_poi_elem, struct page_owner_info, elem);
    free (poi);
  }

  return (list_size (&pg->owners) == 0);
}

/* Shared mappings functions. */

/* Initialize SS with F and ref_count 0. */
void 
shared_mappings_init (struct shared_mappings *sm, struct file *f, int flags)
{
  ASSERT (sm != NULL);
  ASSERT (f != NULL);

  block_sector_t inumber = inode_get_inumber (file_get_inode (f));
  sm->inumber = inumber;

  hash_init (&sm->smi.mappings, shared_mappings_hash_func, shared_mappings_hash_less_func, NULL);
  sm->smi.mmap_file = f;
  sm->smi.flags = flags;
  lock_init (&sm->segment_mapping_info_lock);

  sm->ref_count = 0;
  lock_init (&sm->ref_count_lock);
}

/* Destroy this shared mappings. 
 
   Only call if there are no remaining referrers and we are 
     guaranteed that no new referrers will race with us. 
   This means that all pages must have no owners left. */
void 
shared_mappings_destroy (struct shared_mappings *sm)
{
  ASSERT (sm != NULL);
  ASSERT (sm->ref_count == 0);

  ASSERT (hash_size (&sm->smi.mappings) == 0);
  hash_destroy (&sm->smi.mappings, NULL);
  /* Must do this after destroying the hash table, so that frame table still 
       has the file to work with. */
  if (sm->smi.mmap_file != NULL)
  {
    filesys_lock ();
    file_close (sm->smi.mmap_file);
    filesys_unlock ();
  }
}

/* For use with hash_destroy. */
void 
shared_mappings_destroy_hash_func (struct hash_elem *e, void *aux UNUSED)
{
  ASSERT (e != NULL);

  struct shared_mappings *sm = hash_entry (e, struct shared_mappings, elem);
  ASSERT (sm != NULL);

  shared_mappings_destroy (sm);
}

/* Atomically decrement the ref count of SS.
   If we are the last user, destroy SS. */ 
void 
shared_mappings_decr_ref_count (struct shared_mappings * ss)
{
  ASSERT (ss != NULL);

  lock_acquire (&ss->ref_count_lock);
  ASSERT (0 < ss->ref_count);
  ss->ref_count--;
  lock_release (&ss->ref_count_lock);

  /* If it looks like we're the last user, ask the ro shared segment table
     to remove this segment. */
  if (ss->ref_count == 0)
    ro_shared_mappings_table_remove (ss->smi.mmap_file);
}

/* Atomically increment the ref count of SS.
   If we are the last user, destroy SS. */ 
void 
shared_mappings_incr_ref_count (struct shared_mappings *sm)
{
  ASSERT (sm != NULL);

  lock_acquire (&sm->ref_count_lock);
  /* Could be zero if the last referrer is in the procesm of requesting that 
       ro_shared_mappings_table destroy SS. */
  ASSERT (0 <= sm->ref_count);
  sm->ref_count++;
  lock_release (&sm->ref_count_lock);
}

/* Set SS's fields to hash to KEY. */
void 
shared_mappings_set_hash (struct shared_mappings *sm, unsigned key)
{
  ASSERT (sm != NULL);
  sm->inumber = (block_sector_t) key;
}

/* Hash this shared segment. */
unsigned 
shared_mappings_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  ASSERT (e != NULL);
  struct shared_mappings *sm = hash_entry (e, struct shared_mappings, elem);
  ASSERT (sm != NULL);
  return sm->inumber;
}

/* Which shared_mappings is the lesser? */
bool 
shared_mappings_hash_less_func (const struct hash_elem *a, const struct hash_elem *b, void *aux)
{
  ASSERT (a != NULL);
  ASSERT (b != NULL);
  
  /* KISS. */
  return shared_mappings_hash_func (a, aux) < shared_mappings_hash_func (b, aux);
}

/*
  QUESTION FROM VARUN:
  how can we add and delete info about individual pages ?
  say if a page is evicted out of a frame.
  API's for that 
*/
