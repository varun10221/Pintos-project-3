#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stdint.h>
#include <round.h>

#include "threads/vaddr.h"
#include "devices/block.h"

/* Forward declarations. */
struct frame;

typedef struct frame_swap_table swap_table_t;

enum swap_slot_status
{
  SWAP_SLOT_EMPTY, /* There is no page occupying this slot swap. */
  SWAP_SLOT_OCCUPIED, /* There is a page resident in this slot swap. */
};

/* Entry in the swap table: "swap slot". */
struct swap_slot
{
  id_t id; /* Index into the swap table. */
  struct page *pg; /* Page resident in this swap slot. */
  enum swap_slot_status status; /* Status of this swap slot. */
};

/* Basic life cycle. */
bool swap_table_init (void);
void swap_table_destroy (void);

/* Interface. */
size_t swap_table_store_page (struct page *);
void swap_table_retrieve_page (struct page *, struct frame *);
void swap_table_discard_page (struct page *);

#endif /* vm/swap.h */
