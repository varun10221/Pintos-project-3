#ifndef VM_SWAP_H
#define VM_SWAP_H

#include "vm/frame.h"
#include "devices/block.h"

/* DIV_ROUND_UP in case PGSIZE is not evenly divisible by SECTOR_SIZE. 
   Overestimate the number of sectors we need, since we can't share sectors
   between two slots. */
static const uint32_t BLOCK_SECTORS_PER_PAGE = DIV_ROUND_UP (PGSIZE, SECTOR_SIZE);
static const uint32_t SWAP_TABLE_N_SLOTS = block_size (block_get_role (BLOCK_SWAP)) / BLOCK_SECTORS_PER_PAGE;

#endif /* vm/swap.h */
