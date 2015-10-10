#include "vm/frame.h"

#include <stdint.h>
#include <debug.h>
#include <list.h>

#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "vm/swap.h"

/* System-wide frame table. List of entries containing the resident pages
   Processes use the functions defined in frame.h to interact with this table. */
frame_table_t system_frame_table;

/* Convert KADDR to frame id. TODO */
static id_t 
frame_table_addr_to_ix (void *kaddr)
{
  ASSERT (kaddr != NULL);
  ASSERT ( (uint32_t) kaddr % PGSIZE == 0);

  return 0;
}

/* Initialize this frame. */
static void 
frame_table_init_frame (struct frame *fr, id_t id)
{
  ASSERT (fr != NULL);

  fr->id = id;
  lock_init (&fr->lock);
  fr->status = FRAME_EMPTY;
  fr->pg = NULL;
}

/* Basic life cycle. */

/* Initialize the system_frame_table. Not thread safe. Should be called once. */
void
frame_table_init (void)
{
  size_t i;

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

/* Put this page into a frame.
   The frame's information is stored in PG for the caller
   to deal with.
   TODO */
void 
frame_table_store_page (struct page *pg)
{
  ASSERT (pg != NULL);
    /*acquire global lock*/
   /*do a bitscan of frame_bit map,by calling free_frame _find*/
   /*acquire lock on entry */ /*check frame status*/
   /* release global lock */
  /*in free frame, do a struct frame update */
   /*update the details in corresponding page struct*/
  /*flip the bitmap*/
   /*update in spt */


}

/* Release resources associated with this page; process is done with it. 
   If an mmap'd file, flush changes to backing file. 
   TODO */
void 
frame_table_release_page (struct page *pg)
{ ASSERT (pg != NULL);
  /* Get the frame from  pg->location */
  /*revert  the frame parameters, probably a  frame_init */
  /*acquire lock and do a bitflip */
  /*make the other parameters to match init in case of release due to process exit */
 /*TODO:Can we have a separate function for writing mmap file? */
 /* Im assuming writing back mmap file involves block_write and some other support 
   infra needed */
 
}

/*searches the global frame table and retieves a free frame.Not thread safe */
struct frame *
free_frame_find (void)
{
  /*do a bit map scan and retrieve the first free frame after locking the table */
  /*release the lock*
 /*then call swap out to put the frame in swap and retuen the free index */

   return frame;
}


/*searches the global table and evicts the first non-pinned frame*/
static id_t
frame_evict (void)
{
  /*get a global lock on table*/
  /*search for first non-pinned frame  and  call swap_table_store_page*/
 /*for mmap file range, check if the page is dirty ,if so write to file,
   else set the frame free and return frame */
  /*may need to panic in an unlikely scenario of all frames pinned */
 return id; //variable of type id_t  
}

/* (Put page PG into a frame and) pin  it there. 
   It will not be evicted until a call to 
    - frame_table_release_page(), or
    - frame_table_unpin_page()
   TODO. */
void 
frame_table_pin_page (struct page *pg)
{
  ASSERT (pg != NULL);
 /*change the frame_status to pinned */
}

/* Allow page PG to be evicted from its frame,
   if it is in a frame. 
   TODO. */
void 
frame_table_unpin_page (struct page *pg)
{
  ASSERT (pg != NULL);
}


#if 0
Just an overview of functions so far, 
(to make sure if we have API's to handle all scenario)
i)frame table init and frame init.
ii)functions to store and evict a frame
iii)function to discard frames
iv)functions to pin , unpin.
v)bit map_updation as a part of store and evict.
vi)connecting swap and frame, for reading from swap, we can define the logic from spt, for storing , we can use the functions in frame.c

#endif

#if 0

/* Allocating and freeing entries. */

/* Identify a frame table entry whose page can be evicted.
   Return the locked FTE.

   If no candidate is identified, returns NULL.
   to get eviction candidate

 */
struct frame * frame_table_get_eviction_candidate (frame_table_t *ft)
{
  ASSERT (ft != NULL);

  return NULL;
}

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
#endif
