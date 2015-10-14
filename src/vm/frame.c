#include "vm/frame.h" 
#include <stdint.h>
#include <debug.h>
#include <list.h>

#include "vm/swap.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"

/* System-wide frame table. List of entries containing the resident pages
   Processes use the functions defined in frame.h to interact with this table. */
frame_table_t system_frame_table;

/* Private function declarations. */

/* Nice way to get a frame. */
static struct frame * frame_table_obtain_frame (void);

static struct frame * frame_table_find_free_frame (void);

/* Henchmen for the 'nice way'. */
static struct frame * frame_table_make_free_frame (void);
static struct frame * frame_table_get_eviction_victim (void);
static void frame_table_evict_page_from_frame (struct frame *);
static bool frame_table_validate_eviction_victim (struct frame *);
static void frame_table_write_mmap_page_to_file (struct frame *);
static void frame_table_init_frame (struct frame *, id_t);

/* Basic life cycle. */

/* Initialize the system_frame_table. Not thread safe. Should be called once. */
void
frame_table_init (void)
{
  size_t i;

  system_frame_table.n_free_entries = FRAME_TABLE_N_FRAMES;
  system_frame_table.usage = bitmap_create (FRAME_TABLE_N_FRAMES);
  ASSERT (system_frame_table.usage != NULL);
    
  lock_init (&system_frame_table.usage_lock);

  system_frame_table.entries = malloc (FRAME_TABLE_N_FRAMES * sizeof(struct frame));
  ASSERT (system_frame_table.entries != NULL);

  struct frame *frames = (struct frame *) system_frame_table.entries; /* Cleaner than compiler warnings. */
  for (i = 0; i < FRAME_TABLE_N_FRAMES; i++)
    frame_table_init_frame (&frames[i], i);

  /* Initialize the swap table: the ST is the FT's dirty little secret. */
  swap_table_init ();
}

/* Destroy the system frame table. Not thread safe. Should be called once. */
void 
frame_table_destroy (void)
{
  bitmap_destroy (system_frame_table.usage); 
  free (system_frame_table.entries);

  swap_table_destroy ();
}

/* Storing and releasing pages. */

/* Put locked page PG into a frame.
   Update the page directory for each of its owners. */
void 
frame_table_store_page (struct page *pg)
{
  ASSERT (pg != NULL);

  /* Page must not be in a page already. */
  ASSERT (pg->status != PAGE_RESIDENT);

  struct frame *fr = frame_table_obtain_frame ();
  if (fr == NULL)
    PANIC ("frame_table_store_page: Could not allocate a frame."); 
/*TODO:should we lock the page also? */
  /* Tell each about the other. */
  fr->status = FRAME_OCCUPIED;
  fr->pg = pg;
  pg->location = fr;
  fr->popularity = POPULARITY_START;
  pg->status = PAGE_RESIDENT;
  /* TODO Update the page directory of each owner. */
  ASSERT (0 == 1);
  // pagedir_set_page (pd ,pg, fr,  true), is it evn a correct function?;
  /* Page safely in frame. */
  /*TODO: to update location in spt? may not be needed since we update pagedir, but for consistency sake */
  
  lock_release (&fr->lock);
}

/* Release resources associated with locked page PG; process is done with it. 
   If an mmap'd file, flush changes to backing file. 
   TODO */
void 
frame_table_release_page (struct page *pg) 
{ 
  ASSERT (pg != NULL);
  ASSERT (pg->status == PAGE_RESIDENT);
  /* Only the final owner can release a page. */
  ASSERT (list_size (&pg->owners) == 1);

  /* TODO Hmm? I think that:
       Since we've locked the page, we shouldn't need to lock the frame too. 
       The eviction function should lock frame first, then page. By holding 
       the lock on the page we "skip" the frame locking step. 
       TODO This means that in eviction we must double-check that the page is still
       resident before we actually evict. */
  struct frame * fr = (struct frame *) pg->location;
  ASSERT (fr != NULL);
  ASSERT (fr->status != FRAME_OCCUPIED);
  ASSERT (fr->pg == pg);

  struct page_owner_info *poi = list_entry (list_front (&pg->owners), struct page_owner_info, elem);
  ASSERT (poi != NULL);
  bool need_to_write_back = (pg->smi->mmap_file && pagedir_is_dirty (poi->owner->pagedir, fr->paddr));

  if (need_to_write_back)
  {
    /*  mmap: Write page to file. Should call the same function used by
        frame_table_evict_page_from_frame (victim).is there a way to write just the page in file , how to do that*/
     frame_table_write_mmap_page_to_file (fr);
   }

 /* Can we have a separate function for writing mmap file? 
   Im assuming writing back mmap file involves block_write and some other support 
   infra needed 
   
   JD: Yes, I agree. I think separate functions for evicting mmap and non-mmap pages are needed.
   Please add declarations for these.
   */

  /* Update frame. */
  fr->status = FRAME_EMPTY;
  fr->pg = NULL;
  fr->popularity = POPULARITY_START;
  lock_release (&fr->lock);

  /* Mark frame as available in the bitmap. */
  lock_acquire (&system_frame_table.usage_lock);
  bitmap_flip (system_frame_table.usage, fr->id);
  lock_release (&system_frame_table.usage_lock);
}


/*TODO writes the frames pg back to mmap file in the disk */
static void
frame_table_write_mmap_page_to_file (struct frame *fr)
{




}

/* (Put locked page PG into a frame and) pin it there. 
   It will not be evicted until a call to 
    - frame_table_release_page(), or
    - frame_table_unpin_page()
    */
void 
frame_table_pin_page (struct page *pg)
{
  ASSERT (pg != NULL);
 /*change the frame_status to pinned */
 /*first allocate the frame for the page if not availble before */
  if (pg->location == NULL)
        frame_table_store_page (pg);
       
  struct frame *fr = (struct frame *) pg->location;
  /*lock so that page doesnt get evicted before pinning */
  ASSERT (fr != NULL);
  lock_acquire (&fr->lock);
  /*Varun:guess we need to assert even if page status is pinned prior */
  fr->status = FRAME_PINNED; 
  lock_release (&fr->lock);  
}

/* Allow locked resident page PG to be evicted from its frame.
    */
void 
frame_table_unpin_page (struct page *pg)
{
  ASSERT (pg != NULL);

  ASSERT (pg->status == PAGE_RESIDENT)
  struct frame *fr = (struct frame *) pg->location;

  ASSERT (fr != NULL);
  ASSERT (fr->status == FRAME_PINNED);

  fr->status = FRAME_OCCUPIED;
}

/* Private helper functions. */

/* This function will evict the page from a frame
   and return the frame to you, locked.
 
   Returns NULL if all frames have their page pinned. */
static struct frame * 
frame_table_make_free_frame (void)
{
  struct frame *victim = frame_table_get_eviction_victim ();
  if (victim != NULL)
    frame_table_evict_page_from_frame (victim);
 
  return victim;
}

/* Identify a victim frame whose page can be evicted.
   Return the locked frame.

   If no candidate is identified, returns NULL. */
static struct frame * 
frame_table_get_eviction_victim (void)
{

   uint32_t i;
   struct frame *frames = (struct frame *) system_frame_table.entries;

   /* Preliminary eviction algorithm: Evict the first frame whose page is not pinned. */
   for (i = 0; i <  FRAME_TABLE_N_FRAMES ; i++)
   {
     /* Optimistic search: search without locks, then lock to verify that it's valid. */
     if (frames[i].status != FRAME_PINNED )
     {
        lock_acquire (&frames[i].lock);
        if (frame_table_validate_eviction_victim (&frames[i]))
          return &frames[i];
        else 
          lock_release (&frames[i].lock);
     }
   }
   
   /*TODO: I'm not returning NULL now, will do once eviction algo. is finalized */ 
   PANIC ("All frames are pinned");
}


/* Returns true if the locked FR is a valid eviction victim
     (i.e. is not pinned). */
static bool 
frame_table_validate_eviction_victim (struct frame *fr)
{
  ASSERT (fr != NULL);
  bool is_pinned = (fr->status == FRAME_PINNED);
  return !is_pinned;
}

/* Evict the page in locked frame FR. */
/*do not call it directly without calling eviction victim routine */
static void 
frame_table_evict_page_from_frame (struct frame *fr)
{
  bool a = false; /*dummy variable for compilation */
  ASSERT (fr != NULL);
  /* -  For mmap'd file page, check if the page is dirty. 
       If so write to file, else set the frame free and return frame.*/
  struct page *p = fr->pg;   
  if (p->smi->mmap_file != NULL) /*mmap check by checking if there is a file associated*/
    {   
      struct page_owner_info *poi = list_entry (list_front (&p->owners),
                                                struct page_owner_info, elem);
      
      if (pagedir_is_dirty (poi->owner->pagedir, fr->paddr))/*TODO:it must be vaddress for pagedir */   
        {
          
            frame_table_write_mmap_page_to_file (fr);
         }
      else
         {
           fr->status = FRAME_EMPTY;
           fr->pg = NULL;
         }
    }  
  else
   {  
    swap_table_store_page (fr->pg);
   } 
    /*relase the lock on the locked frame_obtained */         
    lock_release (&fr->lock);
}

/* Initialize frame FR. */
static void 
frame_table_init_frame (struct frame *fr, id_t id)
{
  ASSERT (fr != NULL);

  fr->id = id;
  lock_init (&fr->lock);
  fr->status = FRAME_EMPTY;
  fr->pg = NULL;
}

/* Allocate a frame for a page. 
 
   Locate a free frame, mark it as used, lock it, and return it.
   May have to evict a page to obtain a free frame.
   If no free or evict-able frames are available, returns null. */
static struct frame * 
frame_table_obtain_frame (void)
{
  struct frame *fr = frame_table_find_free_frame ();
  if (fr == NULL)
    fr = frame_table_make_free_frame ();

  return fr;
}

/* Searches the system frame table and retieves a free locked frame. 
   Returns NULL if no free frames are available (necessitates eviction). */
static struct frame *
frame_table_find_free_frame (void)
{ 
  /* Do a bit map scan and retrieve the first free frame. */
  lock_acquire (&system_frame_table.usage_lock);
  size_t free_frame = bitmap_scan (system_frame_table.usage,0,1,false); 
  lock_release (&system_frame_table.usage_lock);

  /* Was a free frame found? */
  if (free_frame != FRAME_TABLE_N_FRAMES)
  {
    struct frame *frames = (struct frame *) system_frame_table.entries;
    return &frames[free_frame];
  }
  return NULL;
}

/*
Just an overview of functions so far, 
(to make sure if we have API's to handle all scenario)
i)frame table init and frame init.
ii)functions to store and evict a frame
iii)function to discard frames
iv)functions to pin, unpin.
v)bit map_updation as a part of store and evict.
vi)connecting swap and frame, for reading from swap, we can define the logic from spt, for storing , we can use the functions in frame.c
*/

#if 0
/* Verify that id ID is valid. */
static inline void frame_table_validate_id (id_t id)
{
  ASSERT (0 <= id && id < FRAME_TABLE_N_FRAMES);
}

static struct frame * frame_table_id_to_frame (id_t);
static id_t frame_table_frame_to_id (struct frame *);

/* Look up the frame associated with id ID. */
static struct frame * 
frame_table_id_to_frame (id_t id)
{
  frame_table_validate_id (id);
  struct frame *frames = (struct frame *) system_frame_table.entries;
  return &frames[id];
}

/* Look up the id associated with frame FR. */
static id_t 
frame_table_frame_to_id (struct frame *fr)
{
  ASSERT (fr != NULL); 
  return fr->id;
}
#endif

#if 0

/* Allocating and freeing entries. */

/* Swap this frame out. */ 
swap_id_t frame_table_swap_out (struct frame *fte)
{
  ASSERT (fte != NULL);

  return 0;
}

/* Swap this frame in. */
void frame_table_swap_in (struct frame *fte, swap_id_t swap_id)
{
  ASSERT (fte != NULL);
}

/* Not yet fleshed out. */
void frame_table_write_page_to_file (struct frame *fte)
{
  ASSERT (fte != NULL);
}

/* Not yet fleshed out. */
void frame_table_read_page_from_file (struct frame *fte)
{
  ASSERT (fte != NULL);
}

/* Get a frame. Thread safe. 
   
   Returns true if we get a frame, else false. */
bool frame_table_get_frame (frame_table_t *ft, struct page *mapping)
{
  ASSERT (ft != NULL);
  ASSERT (mapping != NULL);

  struct frame *fte;

  lock_acquire (&ft->usage_lock);
  /* Find a free frame in usage if one exists. */
  size_t ix = bitmap_scan_and_flip (ft->usage, 0, 1, 0);
  if (ix != BITMAP_ERROR)
    fte = ft->entries[ix];    
  else
  {
    /* Oops, no free entries. Evict a page. */
    fte = frame_table_evict_page (ft);
    if (fte == NULL)
      return false;
  }

  /* Change which lock I hold. */
  lock_acquire (&fte->lock);
  lock_release (&ft->usage_lock);

  /* Copy relevant fields from mapping. */
  struct page *m = (struct page *) malloc(sizeof(struct page));
  if (m == NULL)
    goto CLEANUP_AND_ERROR;

  m->spt = mapping->spt;
  m->vaddr = mapping->vaddr;
  if (fte->status != FRAME_PINNED)
    fte->status = (mapping->is_pinned ? FRAME_OCCUPIED : FRAME_PINNED);

  /* Register mapping in this frame's list of mappings. */
  list_push_back (&fte->mappings, &m->elem);

  /* Tell mapping its physical mapping. */
  mapping->paddr = fte->page;

  lock_release (&fte->lock);
  return true;

  /* Unlock the fte and return false. */
  CLEANUP_AND_ERROR:
    lock_release (&fte->lock);
    return false;
}

/* Release the frame holding kernel page KPAGE. Thread safe. */
void frame_table_release_frame (struct page *pg)
{
  ASSERT (pg != NULL);

  /* Identify the FTE corresponding to MAPPING. */

  /* Mark the slot as empty. */
  lock_acquire (&ft->usage_lock);

  lock_release (&ft->usage_lock);
}

/* Convert KADDR to frame id. TODO */
static id_t 
frame_table_addr_to_ix (void *kaddr)
{
  ASSERT (kaddr != NULL);
  ASSERT ( (uint32_t) kaddr % PGSIZE == 0);

  return 0;
}


#endif
