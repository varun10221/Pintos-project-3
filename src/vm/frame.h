#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <inttypes.h>
#include <stddef.h>
#include <list.h>
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "vm/page.h"
#include "vm/swap.h"

typedef int32_t frame_id_t;
const uint32_t FRAME_TABLE_N_FRAMES = ( (uint32_t) PHYS_BASE / PGSIZE);

enum frame_status
{
  FRAME_EMPTY,     /* There is no page occupying this frame. */
  FRAME_OCCUPIED, /* There is a page resident in this frame. */
  FRAME_PINNED,  /* There is a page resident in this frame. It is pinned (cannot be evicted). */
};


/* Fundamental unit of the frame table. 
 * TODO UPDATE */
struct frame_table_entry
{
  frame_id_t frame_id; /* Unique ID for this frame. */
  unsigned stamp; /* Incremented each time the resident page is evicted. Solve ABA problem. */
  struct lock lock; /* Lock to control this FTE. */
  enum frame_status status;

  /* TODO I think we should be storing pointers, not our own copies. */
  struct list mappings; /* List of 'struct mapping's: processes that are using this frame. Uses fte_elem from 'struct mapping'. */
  void *page; /* Which page is in this frame? */

  /* Can change this type later if needed... */
  struct list_elem elem; /* For tracking in a list. */
};

struct frame_table
{
  struct bitmap *frame_status; 
  struct lock frame_status_lock;

  /* Array mapping frame index to corresponding entry. */
  struct frame_table_entry *frames;
};

/* Basic life cycle. */
bool frame_table_init (struct frame_table *);
void frame_table_destroy (struct frame_table *);

/* Getting and releasing frames. */
bool frame_table_get_frame (struct frame_table *, struct mapping *);
void frame_table_release_frame (struct frame_table *, struct mapping *);

#endif /* vm/frame.h */
