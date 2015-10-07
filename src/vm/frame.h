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

enum frame_swapslot_status
{
  EMPTY, /* There is no page occupying this frame/slot swap. */
  OCCUPIED, /* There is a page resident in this frame/slot swap. */
  PINNED /* There is a page resident in this frame. It is pinned (cannot be evicted). */
};

/* Fundamental unit of the frame and swap tables. */
struct frame_swap_table_entry
{
  id_t id; /* Unique ID for this frame/swap slot. */
  struct page *pg; /* Page resident in this frame or swap slot. */
  unsigned stamp; /* TODO Need this? Incremented each time the resident page is evicted. Solves ABA problem. */

  struct lock lock; /* Lock to control this FTE. */
  enum frame_swapslot_status status;
};

/* Frame table and swap table use same structure. */
struct frame_swap_table
{
  struct bitmap *usage; /* 0 if available, 1 if in use. */
  struct lock usage_lock; /* For atomic updates to usage. */

  /* Array mapping frame index to corresponding entry. */
  struct frame_swap_table_entry *entries;
};

/* Basic life cycle. */
bool frame_table_init (struct frame_table *);
void frame_table_destroy (struct frame_table *);

/* Getting and releasing frames. */
bool frame_table_get_frame (struct frame_table *, struct mapping *);
void frame_table_release_frame (struct frame_table *, struct mapping *);

#endif /* vm/frame.h */
