#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stdint.h>
#include <round.h>

#include "threads/vaddr.h"
#include "threads/synch.h"

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

/* The global swap table is used to store pages belong to a process
   when it is not using them. It is the "backup" for the frame table. */
struct swap_table
{
  struct bitmap *usage; /* 0 if available, 1 if in use. */
  struct lock usage_lock; /* For atomic updates to usage. */

  uint32_t n_slots; /* Total number of slots. */
  struct swap_slot *slots; /* Array of n_slots slots. */
};

/* Basic life cycle. */
void swap_table_init (void);
void swap_table_destroy (void);

/* Interface. */
void swap_table_store_page (struct page *);
void swap_table_retrieve_page (struct page *, struct frame *);
void swap_table_discard_page (struct page *);

#endif /* vm/swap.h */
