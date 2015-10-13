#include "vm/page.h"

#include <debug.h>
#include <round.h>
#include <stdlib.h>

#include "threads/vaddr.h"

/* Maximum distance from stack we can be in order to count as
   stack growth rather than an invalid memory access. */
#define MAX_VALID_STACK_DISTANCE 31

/* System-wide table of read-only shared segments.
   Processes can share entries in this table instead of maintaining
   their own copy of all of the pages they use. */
struct ro_shared_segment_table ro_shared_segment_table;

/* Private function declarations. */
bool supp_page_table_is_range_valid (struct supp_page_table *, void *, void *);
bool supp_page_table_is_stack_growth (struct supp_page_table *, void *);

/* Initialize the ro shared segment table. Not thread safe. Should be called once. */
void 
ro_shared_segment_table_init (void)
{
  /* TODO */
}

/* Destroy the ro shared segment table. Not thread safe. Should be called once. */
void 
ro_shared_segment_table_destroy (void)
{
  /* TODO */
}

/* Initialize a supplemental page table.
   This SPT begins with only a stack segment. */
void supp_page_table_init (struct supp_page_table *spt)
{
  ASSERT (spt != NULL);
  /* TODO */
}

/* Destroy this supplemental page table, releasing
   all resources it holds. */
void supp_page_table_destroy (struct supp_page_table *spt)
{
  ASSERT (spt != NULL);
  /* TODO */
}

/* Return the page associated with virtual address VADDR.
   If VADDR looks like a stack growth, grow the stack and add a page.

   Returns NULL if no such page (i.e. illegal memory access). */
struct page * supp_page_table_find_page (struct supp_page_table *spt, void *vaddr)
{
  ASSERT (spt != NULL);
  ASSERT (vaddr != NULL);

  void *vpgaddr = ROUND_DOWN ( (uint32_t) vaddr, PGSIZE);
  struct segment *seg = NULL;
  struct page *ret = NULL;

  /* Find the segment to which this address belongs. */
  struct segment *tmp_seg = NULL;
  struct list_elem *e = NULL;
  for (e = list_begin (&spt->segment_list); e != list_end (&spt->segment_list);
       e = list_next (e))
  {
    tmp_seg = list_entry (e, struct segment, elem);
    if (tmp_seg->start <= vpgaddr && vpgaddr < tmp_seg->end)
    {
      seg = tmp_seg;
      break;
    }
  }

  /* Found a matching segment. */
  if (seg)
  {
    /* page_mappings is keyed by page number. */
    int page_num = (vpgaddr - seg->start) / PGSIZE;
    
    /* If shared, lock so that we don't race on lookup/modification. */
    if (seg->type == SEGMENT_SHARED_RO)
      lock_acquire (&seg->page_mappings->lock);

    /* TODO Need to init the hash with hash and equals function, and 
       define a hash_elem that will hash appropriately.
    struct hash_elem *e = hash_find (seg->page_mappings->page_mappings, 
    ret = hash_entry (e, struct page, elem);
    */

    /* Not currently present? Need to add a page. */
    if (ret == NULL)
    {
      /* TODO */
      ASSERT (0 == 1);
      ret = NULL;
    }

    /* Done with this segment. */
    if (seg->type == SEGMENT_SHARED_RO)
      lock_release (&seg->page_mappings->lock);
  }
  /* Did not find a matching segment. Could still be stack growth. */
  else
  {
    bool is_stack_growth = supp_page_table_is_stack_growth (spt, vaddr);
      /* Stack growth? Need to add a page. */

      /* Check if we can do so safely without creeping onto our predecessor. 
         No need to worry about racing with adding or deleting a mapping because
           we don't support process-level threads. */
      if ( is_stack_growth )
      {
        /* TODO */
      }
  }

  return ret;
}

/* Add a {read_only | writable} mapping for mapid MAPID starting at address VADDR.

   MAPID must refer to a valid mapping.
 
   Returns true on success, false if VADDR is 0, is not page-aligned, or overlaps
   any already-mapped pages. */

/* Add a memory mapping to supp page table SPT
     for file F beginning at START with flags FLAGS.
   Returns NULL on failure.

   Caller should NOT free the segment.
   Use supp_page_table_remove_segment() to clean up the segment. */
struct segment * supp_page_table_add_mapping (struct supp_page_table *spt, struct file *f, void *vaddr, int flags)
{
  ASSERT (spt != NULL);
  ASSERT (f != NULL);
  ASSERT (vaddr != NULL);

  /* TODO */
  ASSERT (0 == 1);
}

/* Remove the specified segment from this page table.
   If it's an mmap'd segment, will flush all dirty pages. 
   Will free the memory associated with this segment
   If it's a shared mapping, and if we're the last holder of the 
     pages, will free that memory too. 
   Don't forget to close the file! */
void supp_page_table_remove_segment (struct supp_page_table *spt, struct segment *seg)
{
  ASSERT (spt != NULL);
  ASSERT (seg != NULL);

  /* TODO */
  ASSERT (0 != 1);
}

/* Determine whether or not this range is valid:
    - must start above 0
    - must be in user-space (below PHYS_BASE)
    - must be page-aligned 
    - must not overlap with any existing segments in SPT
    
   Returns true if valid, false else. */
bool supp_page_table_is_range_valid (struct supp_page_table *spt, void *start, void *end)
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
  if (PHYS_BASE < end_addr)
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
bool supp_page_table_is_stack_growth (struct supp_page_table *spt, void *vaddr)
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

/*
  QUESTION FROM VARUN:
  how can we add and delete info about individual pages ?
  say if a page is evicted out of a frame.
  API's for that 
*/
