#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <stdint.h>
#include <inttypes.h>
#include <stddef.h>
#include <list.h>

#include "vm/page.h"
#include "vm/swap.h"

#include "threads/synch.h"
#include "threads/vaddr.h"

enum frame_status
{
  FRAME_EMPTY, /* There is no page occupying this frame. */
  FRAME_OCCUPIED, /* There is a page resident in this frame. */
  FRAME_PINNED /* There is a page resident in this frame. It is pinned (cannot be evicted). */
};

/* Entry in the frame table: "frame". */
struct frame
{
  id_t id; /* Index into the frame table. */
  void *paddr; /* Physical address of this frame. */
  enum frame_status status; /* Status of this frame. */

  struct page *pg; /* Page resident in this frame. */
  int8_t popularity; /* For LRU algorithm. Defaults to POPULARITY_START, incremented/decremented based on access bit. */

  struct lock lock; /* For mutex. */
};

/* The global frame table is used to store 
   pages belonging to a process when it needs to use them. */
struct frame_table
{
  int64_t n_free_frames; /* Number of available frames. Protected by n_free_frames_lock. */
  struct lock n_free_frames_lock; /* For atomic updates to n_free_frames. */

  uint32_t n_frames; /* Total number of frames. */
  struct frame *frames; /* Array of n_frames frames. */
  /* Array of physical pages: n_frames contiguous PGSIZE regions
       of memory obtained by palloc_get_multiple. 
     Each frame refers to one of these physical pages. */
  void *phys_pages;
};

/* Basic life cycle. */
void frame_table_init (size_t);
void frame_table_destroy (void);

/* Storing and releasing pages. */
void frame_table_store_page (struct page *);
void frame_table_release_page (struct page *);

void frame_table_pin_page (struct page *);
void frame_table_unpin_page (struct page *);

/* Replacement Algorithm support function */
void frame_table_popularity_update ()


#endif /* vm/frame.h */
