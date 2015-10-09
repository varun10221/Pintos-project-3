#include "vm/swap.h"

#include "devices/block.h"

/* Swap table. Used as extension of frame table. */
struct frame_swap_table swap_table;

/* DIV_ROUND_UP in case PGSIZE is not evenly divisible by BLOCK_SECTOR_SIZE. 
   Overestimate the number of sectors we need, since we can't share sectors
   between two slots. Yes, this is needlessly paranoid. */
const uint32_t BLOCK_SECTORS_PER_PAGE = DIV_ROUND_UP (PGSIZE, BLOCK_SECTOR_SIZE);

static inline uint32_t get_swap_table_n_slots (void)
{ 
  struct block *blk = block_get_role (BLOCK_SWAP);
  ASSERT (blk != NULL);

  return block_size (blk) / BLOCK_SECTORS_PER_PAGE;/* returns no. of pages swap holds*/
}


/*uint32_t BLOCK_SECTORS_PER_PAGE = get_swap_table_n_slots ();*/


uint32_t TOTAL_PAGES_IN_SWAP = get_swap_table_n_slots ();

/* Basic life cycle */
bool swap_table_init (struct frame_swap_table *st)
{  
   int i;
   ASSERT(st!=NULL);
    st->usage = bitmap_create(get_swap_table_n_slots);
    if(st->usage)
         goto CLEANUP_AND_ERROR;
    lock_init(&st->usage_lock);
    st->entries = (struct frame_swap_table_entry *) 
                           calloc(TOTAL_PAGES_IN_SWAP,
                                sizeof(struct frame_swap_table_entry));
     for (i = 0;i <TOTAL_PAGES_IN_SWAP; i++)
       {
          swap_table_init_slot(&entries[i],i);
       }
      


    CLEANUP_AND_ERROR:
         if(st->usage!= NULL)
             bitmap_destroy(st->usage);
         if(ft->entries ! = NULL)
             free(ft->entries);
           return false;            
                 

    /*TODO: initialize the swap table entries by allocating memory */
     return true;
}

void swap_table_init_slot (struct frame_swap_table_entry *ste , swap_id_t id)
{
   ASSERT(ste !=NULL);
   ste->id = id;
   ste->stamp =0;
   lock_init(&ste->lock);
   ste->status = EMPTY;
   ste->page = NULL;
}

void swap_table_destroy (struct frame_swap_table *st)
{

     ASSERT(st !=NULL); 
     bitmap_destroy(st->usage);
     /*TODO: free the swap table entries */  
     free(ft->entries);
}

/*TODO: DISCARD NEEDS TO BE ADDED PER PAGE */
/*write a page to swap, by looking for a free page
 * if found, write a one page amount of data*/
size_t push_to_swap (void *frame_address, struct frame_swap_table *st )
{
  int i;
  struct block *blk = block_get_role (BLOCK_SWAP);
  ASSERT(block!=NULL);
  lock_acquire (&st->usage_lock);
  size_t free_slot = bitmap_scan_and_flip(st->usage,0,1,false);/* searches for one free slot starting at index 0 */
  ASSERT(free_slot != BITMAP_ERROR);/* TODO:need to find a better way to handle this*/
      
  for(i=0 ; i < BLOCK_SECTORS_PER_PAGE;i++)
     {
       block_write(blk,(free_slot+1)*SECTORS_PER_PAGE+i,frame_address+i*SECTOR_SIZE);
            /*TODO: need to check on sector zero and frame adress used as buffer*/  

/*TODO:add the ste assignments in swap in and swap out*/
 }

    lock_release(&st->usage_lock);/*ToDO:add the entry lock for entries*/
    return free_slot;//may use this to update supp.pg table 
}


void pull_from_stack (struct frame_swap_table *st, 
                              size_t swap_slot, void *addr)
{
   struct block *blk = block_get_role(BLOCK_SWAP);
   ASSERT(blk!=NULL);
   lock_acquire(&st->usage_lock);
   if(bitmap_test (st->usage, swap_slot))
     {
       bitmap_set (st->usage , swap_slot , false);
       for(i=0;i<BLOCK_SECTORS_PER_PAGE;i++)
        {
         block_read(blk,(swap_slot+1)*SECTORS_PER_PAGE+i,addr+i*SECTOR_SIZE);
         }
      } 
   else PANIC("pointed swap_slot is empty");
    lock_release(&st->usage_lock);
    
} 

       /*TODO: change the function names to reflect pintos standard */
 




