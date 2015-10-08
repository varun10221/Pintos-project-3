#include "vm/swap.h"

#include "devices/block.h"

/* Swap table. Used as extension of frame table. */
struct frame_swap_table swap_table;

/* DIV_ROUND_UP in case PGSIZE is not evenly divisible by BLOCK_SECTOR_SIZE. 
   Overestimate the number of sectors we need, since we can't share sectors
   between two slots. */
const uint32_t BLOCK_SECTORS_PER_PAGE = DIV_ROUND_UP (PGSIZE, BLOCK_SECTOR_SIZE);

static inline uint32_t get_swap_table_n_slots (void)
{ 
  struct block *blk = block_get_role (BLOCK_SWAP);
  ASSERT (blk != NULL);

  return block_size (blk) / BLOCK_SECTORS_PER_PAGE;
}

/*uint32_t BLOCK_SECTORS_PER_PAGE = get_swap_table_n_slots ();*/


void swap_table_init(struct frame_swap_table *st)
{
    ASSERT(st!=NULL);
    st->usage = bitmap_create(get_swap_table_n_slots);
    if(st->usage)
         goto CLEANUP_AND_ERROR;    
    lock_init(&st->usage_lock);


}



