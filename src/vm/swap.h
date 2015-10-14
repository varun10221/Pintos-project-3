#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stdint.h>
#include <round.h>

#include "threads/vaddr.h"
#include "devices/block.h"

/* Note: the swap table is a ``secret'' of the frame table. The frame table masks
   the existence of the swap table. 
   
   The supplemental page table should direct all page-related requests to the frame 
   table. The frame table may use the swap table if it wishes. */

/* Forward declarations. */
struct frame;

enum swap_slot_status
{
  SWAP_SLOT_EMPTY, /* There is no page occupying this slot swap. */
  SWAP_SLOT_OCCUPIED, /* There is a page resident in this slot swap. */
};

/* Entry in the swap table: "swap slot". */
struct swap_slot
{
  id_t id; /* Index into the swap table. */
  /* TODO Eliminate status entirely, since it's only got two values and pg is either NULL or not NULL? */
  struct page *pg; /* Page resident in this swap slot, or NULL if no page. */
  enum swap_slot_status status; /* Status of this swap slot. */
};

/* Basic life cycle. */
void swap_table_init (void);
void swap_table_destroy (void);

/* Interface. */
void swap_table_store_page (struct page *);
void swap_table_retrieve_page (struct page *, struct frame *);
void swap_table_discard_page (struct page *);

#endif /* vm/swap.h */
