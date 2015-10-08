#include "vm/frame.h"

#include <stdint.h>
#include <debug.h>
#include <list.h>

#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "vm/swap.h"

/*change struct frame_table_entry to frame _swap_table entry .TODO */

/* Frame table. List of frames containing the resident pages. */
struct frame_swap_table frame_table;

/* Convert KADDR to frame id. TODO */
static frame_id_t frame_table_addr_to_ix (void *kaddr)
{
  ASSERT (kaddr != NULL);
  ASSERT (kaddr % PGSIZE == 0);

  return 0;
}

void frame_table_init_frame (struct frame_table_entry *fte, frame_id_t id)
{
  ASSERT (fte != NULL);

  fte->frame_id = id;
  fte->stamp = 0;
  lock_init (&fte->lock);
  fte->status = FRAME_EMPTY;
  fte->page = NULL;
}

/* System-wide frame table. Processes use the functions
   defined in frame.h to interact with this table. */
struct frame_table system_frame_table;

/* Basic life cycle. */

/* Initialize this ft. Not thread safe. Should be called once. */
bool frame_table_init (struct frame_table *ft)
{
  ASSERT (ft != NULL);

  int i;

  ft->frame_status = bitmap_create (FRAME_TABLE_N_FRAMES);
  if (frame_status == NULL)
    goto CLEANUP_AND_ERROR;
    
  lock_init (&ft->frame_status_lock);

  ft->frames = (struct frame_table_entry *) malloc (FRAME_TABLE_N_FRAMES * sizeof(struct frame_table_entry));
  if (ft->frames == NULL)
    goto CLEANUP_AND_ERROR;
  for (i = 0; i < FRAME_TABLE_N_FRAMES; i++)
  {
    frame_table_init_frame (&frames[i], i);
  }

  return true;

  CLEANUP_AND_ERROR:
    if (ft->frame_status != NULL)
      bitmap_destroy (ft->frame_status):
    if (ft->frames != NULL)
      free (ft->frames);
    return false;
}

/* Destroy this ft. Not thread safe. Should be called once. */
void frame_table_destroy (struct frame_table *ft)
{
  ASSERT (ft != NULL);

  bitmap_destroy (ft->frame_status); 
  free (frames);
}

/* Allocating and freeing frames. */

/* Identify a frame table entry whose page can be evicted.
   Return the locked FTE.

   If no candidate is identified, returns NULL. */
struct frame_table_entry * frame_table_get_eviction_candidate (struct frame_table *ft)
{
  ASSERT (ft != NULL);

  return NULL;
}

/* Swap this frame out. */ 
swap_id_t frame_table_swap_out (struct frame_table_entry *fte)
{
  ASSERT (fte != NULL);

  return 0;
}

/* Swap this frame in. */
void frame_table_swap_in (struct frame_table_entry *fte, swap_id_t swap_id)
{
  ASSERT (fte != NULL);
}

/* Not yet fleshed out. */
void frame_table_write_page_to_file (struct frame_table_entry *fte)
{
  ASSERT (fte != NULL);
}

/* Not yet fleshed out. */
void frame_table_read_page_from_file (struct frame_table_entry *fte)
{
  ASSERT (fte != NULL);
}

/* Get a frame. Thread safe. 
   
   Returns true if we get a frame, else false. */
bool frame_table_get_frame (struct frame_table *ft, struct mapping *mapping)
{
  ASSERT (ft != NULL);
  ASSERT (mapping != NULL);

  struct frame_table_entry *fte;

  lock_acquire (&ft->frame_status_lock);
  /* Find a free frame in frame_status if one exists. */
  size_t ix = bitmap_scan_and_flip (ft->frame_status, 0, 1, 0);
  if (ix != BITMAP_ERROR)
    fte = ft->frames[ix];    
  else
  {
    /* Oops, no free frames. Evict a page. */
    fte = frame_table_evict_page (ft);
    if (fte == NULL)
      return false;
  }

  /* Change which lock I hold. */
  lock_acquire (&fte->lock);
  lock_release (&ft->frame_status_lock);

  /* Register this mapping. */
  fte->stamp++;

  /* Copy relevant fields from mapping. */
  struct mapping *m = (struct mapping *) malloc(sizeof(struct mapping));
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
void frame_table_release_frame (struct frame_table * ft, struct mapping *mapping)
{
  ASSERT (ft != NULL);
  ASSERT (mapping != NULL);

  /* Identify the FTE corresponding to MAPPING. */

  /* Mark the slot as empty. */
  lock_acquire (&ft->frame_status_lock);

  lock_release (&ft->frame_status_lock);
}


