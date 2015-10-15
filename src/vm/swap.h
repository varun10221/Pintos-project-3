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

/* Entry in the swap table: "swap slot". */
struct swap_slot
{
  id_t id; /* Index into the swap table. */
  struct page *pg; /* Page resident in this swap slot, or NULL if no page. */
};

/* Basic life cycle. */
void swap_table_init (void);
void swap_table_destroy (void);

/* Interface. */
void swap_table_store_page (struct page *);
void swap_table_retrieve_page (struct page *, struct frame *);
void swap_table_discard_page (struct page *);

#endif /* vm/swap.h */
