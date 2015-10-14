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
typedef struct frame_swap_table frame_table_t;
frame_table_t system_frame_table;

/* Private function declarations. */

/* Nice way to get a frame. */
static struct frame * frame_table_obtain_locked_frame (void);

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

  struct frame *fr = frame_table_obtain_locked_frame ();
  if (fr == NULL)
    PANIC ("frame_table_store_page: Could not allocate a frame."); 

  /* Tell each about the other. */
  fr->status = FRAME_OCCUPIED;
  fr->pg = pg;
  fr->popularity = POPULARITY_START;
  pg->location = fr;
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
  ASSERT (pg->status != PAGE_DISCARDED);
  /* TODO handle possible page status.
     HOWEVER Make sure you lock the frame if it is resident. 
     Coordinate with frame_table_evict_page_from_frame. */
  /* Only the final owner can release a page.*/
  /* TODO  0 ? */
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
  ASSERT (fr != NULL);
}
/* TODO have  a read mmap from file API */

/* (Put locked page PG into a frame and) pin it there. 
   It will not be evicted until a call to 
    - frame_table_release_page(), or
    - frame_table_unpin_page()
    */
void 
frame_table_pin_page (struct page *pg)
{
  ASSERT (pg != NULL);
  /*first allocate the frame for the page if not availble before */
  if (pg->location == NULL)
    frame_table_store_page (pg);

  /* NB: frame_table_evict_page_from_frame locks page, so there's
     no risk of the page being evicted before we can mark the frame
     as pinned. */
       
  struct frame *fr = (struct frame *) pg->location;
  ASSERT (fr != NULL);
  /*TODO valid assert? Or can it be FRAME_PINNED? */
  ASSERT (fr->status == FRAME_OCCUPIED);
  /* Change the frame_status to pinned. */
  fr->status = FRAME_PINNED; 
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
  {
    frame_table_evict_page_from_frame (victim);
  }
 
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
   
   return NULL;
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
static void 
frame_table_evict_page_from_frame (struct frame *fr)
{
  ASSERT (fr != NULL);

  /* TODO Coordinate with frame_table_release_page. */

  /* If this frame is empty, nothing to do. */
  if (fr->status == FRAME_EMPTY)
    return;
  ASSERT (fr->status == FRAME_OCCUPIED);

  /*   For mmap'd file page, check if the page is dirty. 
       If so write to file, else set the frame free and return frame.*/
  struct page *pg = fr->pg;   

  /* Synchronize with frame_table_pin_page. */
  lock_acquire (&pg->lock);
  ASSERT (pg->status == PAGE_RESIDENT);

  /* TODO Need to update each owner's pagedir so that they page fault when they next access this page. 
     Do this FIRST so that the owners will page fault on access and have to wait for us. */

  if (pg->smi->mmap_file != NULL) /*mmap check by checking if there is a file associated*/
  {   
    /* TODO Need to check EVERY owner's pagedir. Review
       4.1.5.1 Accessed and Dirty Bits. */
    bool page_dirty = false;
    struct page_owner_info *poi = list_entry (list_front (&pg->owners),
                                              struct page_owner_info, elem);
    
    if (pagedir_is_dirty (poi->owner->pagedir, fr->paddr)) /*TODO:it must be vaddress for pagedir */   
    {
      page_dirty = true;
    }

    if (page_dirty)
      frame_table_write_mmap_page_to_file (fr);
  }  
  else
    swap_table_store_page (pg);


  /* TODO make sure pg->status is set appropriately by swap_table_store_page and frame_table_write_mmap_page_to_file. */
  ASSERT (pg->status != PAGE_RESIDENT);

  /* Now this frame is empty. */
  fr->status = FRAME_EMPTY;
  fr->pg = NULL;

  lock_release (&pg->lock);
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
frame_table_obtain_locked_frame (void)
{
  struct frame *fr = frame_table_find_free_frame ();
  if (fr == NULL)
    fr = frame_table_make_free_frame ();

  return fr;
}

/* Searches the system frame table and retrieves a free locked frame. 
   Returns NULL if no free frames are available (necessitates eviction). */
static struct frame *
frame_table_find_free_frame (void)
{ 
  /* Do a bit map scan and retrieve the first free frame. */
  lock_acquire (&system_frame_table.usage_lock);
  size_t free_frame = bitmap_scan (system_frame_table.usage,0,1,false); 
  lock_release (&system_frame_table.usage_lock);

  /* Was a free frame found? */
  if (0 <= free_frame && free_frame < FRAME_TABLE_N_FRAMES)
  {
    struct frame *frames = (struct frame *) system_frame_table.entries;
    return &frames[free_frame];
  }
  return NULL;
}
