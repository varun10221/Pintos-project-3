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

#include <bitmap.h>

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

/* Frame table and swap table have the same structure. */
struct frame_swap_table
{
  int64_t n_free_entries; /* Number of available entries. Protected by usage_lock. */
  struct bitmap *usage; /* 0 if available, 1 if in use. */
  struct lock usage_lock; /* For atomic updates to usage. */

  /* Array mapping frame index to corresponding entry. 
     Frame table: struct frame*
     Swap table: struct swap_slot* */
  void *entries;

  /* Frame table allocates frames: X contiguous PGSIZE regions
     of memory obtained by palloc_get_multiple. */
  /* TODO This difference is sufficient to require making a separate
     frame table and swap table definition. Let's do that. */
  void *frames;
  uint32_t n_frames;
};

/* Basic life cycle. */
void frame_table_init (size_t);
void frame_table_destroy (void);

/* Storing and releasing pages. */
void frame_table_store_page (struct page *);
void frame_table_release_page (struct page *);

void frame_table_pin_page (struct page *);
void frame_table_unpin_page (struct page *);

#endif /* vm/frame.h */
