#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <inttypes.h>
#include <stddef.h>
#include <list.h>

#include "vm/page.h"
#include "vm/swap.h"

#include "threads/synch.h"
#include "threads/vaddr.h"

typedef int32_t id_t;
static const uint32_t FRAME_TABLE_N_FRAMES = ( (uint32_t) PHYS_BASE / PGSIZE);

enum frame_table_entry_status
{
  EMPTY, /* There is no page occupying this frame. */
  OCCUPIED, /* There is a page resident in this frame. */
  PINNED /* There is a page resident in this frame. It is pinned (cannot be evicted). */
};

/* Entry in the frame table: "frame". */
struct frame
{
  id_t id; /* Index into the frame table. */

  struct page *pg; /* Page resident in this frame. */
  struct void *paddr; /* Physical address of this frame. */

  int8_t popularity; /* For LRU algorithm. Defaults to POPULARITY_START, incremented/decremented based on access bit. */

  struct lock lock; /* Lock to control this FTE. */
  enum frame_table_entry_status status; /* Status of this frame. */
};

/* Frame table and swap table have the same structure. */
struct frame_swap_table
{
  struct bitmap *usage; /* 0 if available, 1 if in use. */
  struct lock usage_lock; /* For atomic updates to usage. */

  /* Array mapping frame index to corresponding entry. 
     Frame table: struct frame*
     Swap table: struct swap_slot* */
  void *entries;
};

/* Basic life cycle. */
bool frame_table_init (struct frame_swap_table *);
void frame_table_destroy (struct frame_swap_table *);

/* Getting and releasing frames. */
bool frame_table_get_frame (struct frame_swap_table *, struct page *);
void frame_table_release_frame (struct frame_swap_table *, struct page *);

#endif /* vm/frame.h */
