#include "vm/frame.h" 
#include <stdint.h>
#include <debug.h>
#include <list.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "vm/swap.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "vm/page.h"

/* System-wide frame table. List of.frames containing the resident pages
   Processes use the functions defined in frame.h to interact with this table. */
struct frame_table system_frame_table;

/* Private function declarations. */
static void frame_table_init_frame (struct frame *, id_t);

/* Nice way to get a frame. */
static struct frame * frame_table_obtain_locked_frame (void);

/* Henchmen for the 'nice way'. */
static struct frame * frame_table_find_free_frame (void);
static struct frame * frame_table_make_free_frame (void);
static struct frame * frame_table_get_eviction_victim (void);
static void frame_table_evict_page_from_frame (struct frame *);
static void frame_table_write_mmap_page_to_file (struct page *, struct frame *);
static void frame_table_read_mmap_page_from_file (struct page *, struct frame *);

/* Initialize the system_frame_table. Not thread safe. Should be called once. */
void
frame_table_init (size_t n_frames)
{
  size_t i;
  n_frames -= 10; /* Allow processes to load small executables. TODO TESTING ONLY. */

  system_frame_table.n_frames = n_frames;
  system_frame_table.n_free_frames = system_frame_table.n_frames;
  system_frame_table.usage = bitmap_create (system_frame_table.n_frames);
  ASSERT (system_frame_table.usage != NULL);
    
  lock_init (&system_frame_table.usage_lock);
  /* Allocate frames: list of struct frames. */
  system_frame_table.frames = malloc (system_frame_table.n_frames * sizeof(struct frame));
  ASSERT (system_frame_table.frames != NULL);
  /* Allocate phys_pages: system_frame_table.n_frames contiguous PGSIZE regions. */
  system_frame_table.phys_pages = palloc_get_multiple (PAL_USER, system_frame_table.n_frames);
  ASSERT (system_frame_table.phys_pages != NULL);

  /* Initialize each frame. */
  struct frame *frames = (struct frame *) system_frame_table.frames;
  for (i = 0; i < system_frame_table.n_frames; i++)
    frame_table_init_frame (&frames[i], i);

  /* Initialize the swap table: the ST is the FT's dirty little secret. */
  swap_table_init ();
}

/* Destroy the system frame table. Not thread safe. Should be called once. */
void 
frame_table_destroy (void)
{
  bitmap_destroy (system_frame_table.usage); 
  free (system_frame_table.frames);
  palloc_free_multiple (system_frame_table.phys_pages, system_frame_table.n_frames);

  swap_table_destroy ();
}

/* Storing and releasing pages. */

/* Put page PG into a frame.
   (includes loading the page's contents into the frame).
   Update the page directory for each of its owners. 
   Acquires and releases lock on PG. */
void 
frame_table_store_page (struct page *pg)
{
  ASSERT (pg != NULL);
  lock_acquire (&pg->lock);

  /* Page must not be in a page already. */
  ASSERT (pg->status != PAGE_RESIDENT);

  struct frame *fr = frame_table_obtain_locked_frame ();
  if (fr == NULL)
    PANIC ("frame_table_store_page: Could not allocate a frame."); 
  ASSERT (lock_held_by_current_thread (&fr->lock));

  /*  Loading page contents into frame (swap in, zero out frame, mmap in, etc. depending on pg->status). */
  switch (pg->status)
  {
    case PAGE_IN_FILE:
      frame_table_read_mmap_page_from_file (pg, fr);
      break;
    case PAGE_SWAPPED_OUT:
      swap_table_retrieve_page (pg, fr);
      break;
    case PAGE_STACK_NEVER_ACCESSED:
      memset (fr->paddr, 0 , PGSIZE);
      break;
    default:
      PANIC ("frame_table_store_page: invalid page state"); 
  }
  
  /* Tell each about the other. */
  fr->status = FRAME_OCCUPIED;
  fr->pg = pg;
  fr->popularity = POPULARITY_START;

  pg->status = PAGE_RESIDENT;
  pg->location = fr;

  /* Update the page directory of each owner. */
  page_update_owners_pagedir (pg, fr->paddr);

  /* Page safely in frame. */
  lock_release (&fr->lock);
  lock_release (&pg->lock);
}

/* Release resources associated with resident locked page PG; it has no owners.
   If a dirty page in an mmap'd file, flush changes to backing file. 
   We acquire and release the lock on the frame in which PG is resident. */
void 
frame_table_release_page (struct page *pg) 
{ 
  ASSERT (pg != NULL);
  ASSERT (lock_held_by_current_thread (&pg->lock));

  ASSERT (pg->status == PAGE_RESIDENT);
  /* A page is only released when it has no owners left. */
  ASSERT (list_size (&pg->owners) == 0);

  struct frame *fr = (struct frame *) pg->location;
  ASSERT (fr != NULL);
  lock_acquire (&fr->lock);
  ASSERT (fr->status == FRAME_OCCUPIED);
  ASSERT (fr->pg == pg);

  bool is_dirty = page_unset_dirty (pg);
  bool need_to_write_back = (pg->smi->mmap_file && is_dirty);
  if (need_to_write_back)
    frame_table_write_mmap_page_to_file (pg, fr);

  /* Update frame. */
  fr->status = FRAME_EMPTY;
  fr->pg = NULL;
  fr->popularity = POPULARITY_START;
  lock_release (&fr->lock);

  /* Update page. */
  pg->status = FRAME_DISCARDED;
  pg->location = NULL;

  /* Mark frame as available in the bitmap. */
  lock_acquire (&system_frame_table.usage_lock);
  bitmap_flip (system_frame_table.usage, fr->id);
  lock_release (&system_frame_table.usage_lock);
}

/*  Writes the locked frame  and locked pg back to mmap file in the disk */
/*  Checks if the page is last, in which case it writes the content size in last page */
/*  Else it will write in page_sizes */
static void
frame_table_write_mmap_page_to_file (struct page *pg, struct frame *fr)
{
  ASSERT (pg != NULL);
  ASSERT (lock_held_by_current_thread (&pg->lock));
  ASSERT (fr != NULL);
  ASSERT (lock_held_by_current_thread (&fr->lock));

  /* Make sure there is agreement. */
  ASSERT (fr->pg == pg);
  ASSERT ((struct frame *) pg->location == fr);
  
  /* Find the length of the mmap file */
  size_t file_len = file_length (pg->smi->mmap_file);
  size_t size;

  /* If file_length is a multiple of page size, then last page in file is of page size */
  if (file_len % PGSIZE != 0)
    {
      /* Condition to check if the page is last page in file or not */
     bool is_last_page_in_file = (file_len - (pg->segment_page)*PGSIZE 
                                                    < PGSIZE) ? true : false;
                                    
     /* Check if its last page in file, as the last page's size may  less than pg size;*/
     if (is_last_page_in_file)
       size = file_len % PGSIZE;
     else 
       size = PGSIZE;  
    }
  /* Determine the size to write in file, PGSIZE */ 
  else 
    size = PGSIZE;
  /* Write the mmap page to file , in the page's respective position in the file */
  filesys_lock ();
  file_write_at (pg->smi->mmap_file, fr->paddr, size,
                             (pg->segment_page)*PGSIZE);
  filesys_unlock ();

  pg->status = PAGE_IN_FILE;
  pg->location = NULL;
}

/* Read the mmap page from file in to the frame */
/*Requires that both page and frame to be locked */
static void
frame_table_read_mmap_page_from_file (struct page *pg, struct frame *fr)
{
  ASSERT (pg != NULL);
  ASSERT (lock_held_by_current_thread (&pg->lock));
  ASSERT (fr != NULL);
  ASSERT (lock_held_by_current_thread (&fr->lock));

  ASSERT (pg->status == PAGE_IN_FILE);
  ASSERT (fr->status == FRAME_EMPTY);

  /* Find the length of the mmap file */
  size_t file_len = file_length (pg->smi->mmap_file);
  size_t size;

  /* If file_length is a multiple of page size, then last page in file is of page size */

  if (file_len % PGSIZE != 0)
    {
      /* Condition to check if the page is last page in file or not */
     bool is_last_page_in_file = (file_len - (pg->segment_page)*PGSIZE
                                                    < PGSIZE) ? true : false;

     /* Check if its last page in file, as the last page's size may  less than pg size;*/
     if (is_last_page_in_file)
       size = file_len % PGSIZE;
     else size = PGSIZE;
    }
  /* Determine the size to write in file, PGSIZE */
  else 
    size = PGSIZE;

  /* Write the mmap page to file , in the page's respective position in the file */
  filesys_lock ();
  file_read_at (pg->smi->mmap_file, fr->paddr, size,
                             (pg->segment_page)*PGSIZE);
  filesys_unlock ();

  /* If we didn't read a full page, zero out the rest. */
  memset(fr->paddr + size, 0, PGSIZE - size);

  /* Update statuses. */
  fr->status = FRAME_OCCUPIED;
  fr->pg = pg;

  pg->status = PAGE_RESIDENT;
  pg->location = fr;
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
  /* TODO Is this safe from race conditions? I think frame needs to be locked the entire time. */
  /* First allocate the frame for the page if not available before */
  if (pg->location == NULL)
    frame_table_store_page (pg);

  /* NB: frame_table_evict_page_from_frame locks page, so there's
     no risk of the page being evicted before we can mark the frame
     as pinned. */
       
  struct frame *fr = (struct frame *) pg->location;
  ASSERT (fr != NULL);
  /*  Assuming frame can only be occupied and not FRAME_PINNED or FRAME_EMPTY */
  ASSERT (fr->status == FRAME_OCCUPIED);
  /* Change the frame_status to pinned. */
  fr->status = FRAME_PINNED; 
}

/* Allow locked resident page PG to be evicted from its frame. */
void 
frame_table_unpin_page (struct page *pg)
{
  ASSERT (pg != NULL);
  ASSERT (lock_held_by_current_thread (&pg->lock));
  ASSERT (pg->status == PAGE_RESIDENT)

  struct frame *fr = (struct frame *) pg->location;
  ASSERT (fr != NULL);

  /* Acquiring and releasing this lock is probably paranoid, given
     that this frame won't be selected for eviction because it is in state FRAME_PINNED. */
  lock_acquire (&fr->lock);

  ASSERT (fr->status == FRAME_PINNED);
  ASSERT (fr->pg == pg);
  fr->status = FRAME_OCCUPIED;

  lock_release (&fr->lock);
}

/* Private helper functions. */

/* This function will evict the page from a frame
   and return the frame to you, locked.
 
   Returns NULL if all frames have their page pinned
     or no victims could be identified. See frame_table_get_eviction_victim. */
static struct frame * 
frame_table_make_free_frame (void)
{
  struct frame *victim = frame_table_get_eviction_victim ();
  if (victim != NULL)
  {
    ASSERT (lock_held_by_current_thread (&victim->lock));
    frame_table_evict_page_from_frame (victim);
  }
 
  return victim;
}

/* Identify a victim frame whose resident page (if any) is locked and can be evicted.
   Return the locked frame containing its locked page (if any).

   If no candidate is identified, returns NULL. */
static struct frame * 
frame_table_get_eviction_victim (void)
{

   uint32_t i;
   struct frame *frames = (struct frame *) system_frame_table.frames;

   /* Preliminary eviction algorithm: Evict the first frame whose page is not pinned. */

   /* Try up to 5 times. We skip frames that contain a locked page, even if the page is evictable.
      So "try again". In practice this is unlikely to affect us, since a process can have at most
      one locked page at a time and there are far more frames than (expected) processes. */
   int counter;
   for (counter = 0; counter < 5; counter++)
   {
     for (i = 0; i <  system_frame_table.n_frames ; i++)
     {
       /* Optimistic search: search without locks, then lock to verify that it's valid. */
       if (frames[i].status != FRAME_PINNED )
       {
          struct frame *fr = &frames[i];
          lock_acquire (&fr->lock);

          /* Can only evict non-pinned pages. */
          if (fr->status != FRAME_PINNED)
          {
            /* If occupied, can only evict if we can lock the page. */
            if (fr->status == FRAME_OCCUPIED)
            {
              if (lock_try_acquire (&fr->pg->lock))
                return fr;
            }
            /* No resident, so an empty frame. */
            else
              return fr;
          }
          /* OK, guess we didn't find a candidate. Release and try again. */
          lock_release (&fr->lock);
       }

     } /* Loop over all frames. */
   } /* Loop from 0 to 5. */
   
   return NULL;
}


/* Evict the page in locked frame FR. 
   FR may be empty.
   FR may have a resident page, in which case the page
     is also locked and should be unlocked before returning.  */
static void 
frame_table_evict_page_from_frame (struct frame *fr)
{
  ASSERT (fr != NULL);

  /* If this frame is empty, nothing to do. */
  if (fr->status == FRAME_EMPTY)
    return;
  ASSERT (fr->status == FRAME_OCCUPIED);

  /*   For mmap'd file page, check if the page is dirty. 
       If so write to file, else set the frame free and return frame.*/
  struct page *pg = fr->pg;   

  /* Synchronize with frame_table_pin_page. */
  ASSERT (pg->status == PAGE_RESIDENT);

  /* Update each owner's pagedir so that they page fault when they next access this page. 
     Do this BEFORE copying the contents so that the owners will page fault on access and have to wait for us
     to finish evicting it. */
  page_clear_owners_pagedir (pg);

  if (pg->smi->mmap_file != NULL)
  {   
    /* mmap (uses file as backing store): only need to write back if page is dirty. 
       Once we've written it out, PG is no longer dirty. */
    if (page_unset_dirty (pg))
      frame_table_write_mmap_page_to_file (pg, fr);
  }  
  else
    /* Not mmap: store in swap table. */
    swap_table_store_page (pg);

  ASSERT (pg->status != PAGE_RESIDENT);

  /* Update frame status. */
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
  /* frames is contiguous memory, so linear addressing works. */
  fr->paddr = system_frame_table.phys_pages + id*PGSIZE;
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

  if (fr != NULL)
  {
    ASSERT (lock_held_by_current_thread (&fr->lock));
  }

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
  if (0 <= free_frame && free_frame < system_frame_table.n_frames)
  {
    struct frame *frames = (struct frame *) system_frame_table.frames;
    struct frame *fr = &frames[free_frame];
    lock_acquire (&fr->lock);
    return fr;
  }

  return NULL;
}
