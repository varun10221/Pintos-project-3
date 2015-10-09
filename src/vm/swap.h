#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <round.h>

#include "vm/frame.h"
#include "threads/vaddr.h"
#include "devices/block.h"

enum swap_table_entry_status
{
  EMPTY, /* There is no page occupying this slot swap. */
  OCCUPIED, /* There is a page resident in this slot swap. */
};

/* Entry in the swap table: "swap slot". */
struct swap_slot
{
  id_t id; /* Index into the swap table. */

  struct page *pg; /* Page resident in this swap slot. */

  struct lock lock; /* Lock to control this FTE. */
  enum swap_table_entry_status status; /* Status of this swap slot. */
};

/* Basic life cycle. */
bool swap_table_init (struct frame_swap_table *);
void swap_table_destroy (struct frame_swap_table *);

/* TODO Declare public interface. */

#endif /* vm/swap.h */
