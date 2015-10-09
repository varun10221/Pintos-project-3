#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <round.h>

#include "vm/frame.h"
#include "threads/vaddr.h"
#include "devices/block.h"

/* Basic life cycle. */
bool swap_table_init (struct frame_swap_table *);
void swap_table_destroy (struct frame_swap_table *);

/* TODO Declare public interface. */

#endif /* vm/swap.h */
