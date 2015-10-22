#include "vm/swap.h"

#include <bitmap.h>

#include "vm/frame.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "devices/block.h"

/* System swap table. Serves as an extension of the frame table. 
   List of pages that are stored on disk instead of in memory. */
struct swap_table system_swap_table;

/* DIV_ROUND_UP in case PGSIZE is not evenly divisible by BLOCK_SECTOR_SIZE. 
   Overestimate the number of sectors we need, since we can't share sectors
   between two slots. Yes, this is needlessly paranoid. */
const uint32_t BLOCK_SECTORS_PER_PAGE = DIV_ROUND_UP (PGSIZE, BLOCK_SECTOR_SIZE);

/* Globals set in swap_table_init. */
uint32_t SWAP_TABLE_N_SLOTS = 0;
struct block *SWAP_BLOCK = NULL;

/* Private function APIs. */
static uint32_t get_swap_table_n_slots (void);
static void swap_table_init_swap_slot (struct swap_slot *, id_t);

/* Definitions. */

/* Return the total number of slots in the swap table. */
static uint32_t 
get_swap_table_n_slots (void)
{ 
  struct block *blk = block_get_role (BLOCK_SWAP);
  ASSERT (blk != NULL);

  return block_size (blk) / BLOCK_SECTORS_PER_PAGE;
}

/* Initialize this swap slot. */
static void 
swap_table_init_swap_slot (struct swap_slot *slot, id_t id)
{
  ASSERT (slot != NULL);

  slot->id = id;
  slot->pg = NULL;
}

/* Basic life cycle */

/* Initialize the system swap table. */
void 
swap_table_init (void)
{
  /* Set globals. */
  SWAP_TABLE_N_SLOTS = get_swap_table_n_slots ();
  SWAP_BLOCK = block_get_role (BLOCK_SWAP);
  ASSERT (SWAP_BLOCK != NULL);

  /* Bitmap. */
  system_swap_table.usage = bitmap_create (SWAP_TABLE_N_SLOTS);
  ASSERT (system_swap_table.usage != NULL);

  lock_init (&system_swap_table.usage_lock);

  /* Slots. */
  system_swap_table.slots = (struct swap_slot *) 
                                malloc (SWAP_TABLE_N_SLOTS 
                                  * sizeof(struct swap_slot));
  ASSERT (system_swap_table.slots != NULL);

  struct swap_slot *slots = (struct swap_slot *) system_swap_table.slots; /* Cleaner than compiler warnings. */
  size_t i;
  for (i = 0; i < SWAP_TABLE_N_SLOTS; i++)
    swap_table_init_swap_slot ((struct swap_slot *) &slots[i], i);
}

/* Destroy this swap table. */
void 
swap_table_destroy (void)
{
  bitmap_destroy (system_swap_table.usage);
  free (system_swap_table.slots);
}

/* Write locked PG to a free swap slot. Panic if no available slots. 
   Stores swap slot information in PG. 
   Caller must hold lock on PG. */
void 
swap_table_store_page (struct page *pg)
{
  ASSERT (pg != NULL);
  ASSERT (lock_held_by_current_thread (&pg->lock));

  /* Make sure PG is in a correct state. */
  ASSERT (pg->status == PAGE_RESIDENT);
  struct frame *curr_frame = (struct frame *) pg->location;
  ASSERT (curr_frame->pg == pg);
  ASSERT (curr_frame->status == FRAME_OCCUPIED);

  /* Find a free slot. */
  lock_acquire (&system_swap_table.usage_lock);
  size_t free_slot = bitmap_scan_and_flip (system_swap_table.usage, 0, 1, false);
  if (free_slot == BITMAP_ERROR)
    PANIC("swap_table_store_page: No available swap slots!");
  lock_release (&system_swap_table.usage_lock);

  /* Make sure slot is in a correct state. */
  struct swap_slot *slots = (struct swap_slot *) system_swap_table.slots; /* Cleaner than compiler warnings. */
  struct swap_slot *s = &slots[free_slot];
  ASSERT ((size_t) s->id == free_slot); /* Paranoia. */

  /* Update slot state and contents. */
  s->pg = pg;
      
  size_t i;
  for (i = 0; i < BLOCK_SECTORS_PER_PAGE; i++)
  {
     /* Is this the right invocation for block_write? Need to check on sector zero and frame adress used as buffer. */
     block_write (SWAP_BLOCK, i + free_slot*BLOCK_SECTORS_PER_PAGE, curr_frame->paddr + i*BLOCK_SECTOR_SIZE);
  }

  /* Update page info. */
  pg->status = PAGE_SWAPPED_OUT;
  pg->location = s;
}

/* Retrieve locked PG from its swap slot. Put the page contents into locked empty frame FR. */
void 
swap_table_retrieve_page (struct page *pg, struct frame *fr)
{
  ASSERT (pg != NULL);
  ASSERT (lock_held_by_current_thread (&pg->lock));
  ASSERT (fr != NULL);
  ASSERT (lock_held_by_current_thread (&fr->lock));
  ASSERT (fr->status == FRAME_EMPTY);

  ASSERT (pg->status == PAGE_SWAPPED_OUT);
  struct swap_slot *s = (struct swap_slot *) pg->location;
  ASSERT (s->pg == pg);

  /* Read slot into frame. */
  size_t i;
  for (i = 0; i < BLOCK_SECTORS_PER_PAGE; i++)
    block_read (SWAP_BLOCK, i + s->id*BLOCK_SECTORS_PER_PAGE, fr->paddr + i*BLOCK_SECTOR_SIZE);

  /* Wipe the slot. */
  s->pg = NULL;

  /* Mark slot as available. */
  lock_acquire (&system_swap_table.usage_lock);
  ASSERT (bitmap_test (system_swap_table.usage, s->id));
  bitmap_set (system_swap_table.usage, s->id, false);
  lock_release (&system_swap_table.usage_lock);

  /* Update page info. */
  pg->status = PAGE_RESIDENT;
  pg->location = fr;
} 

/* Free up the swap slot used by locked page PG. */
void 
swap_table_discard_page (struct page *pg)
{
  ASSERT (pg != NULL);
  ASSERT (lock_held_by_current_thread (&pg->lock));

  /* Page must be swapped out. */
  ASSERT (pg->status == PAGE_SWAPPED_OUT);
  /* Only the final owner can release a page. */
  ASSERT (list_size (&pg->owners) == 0);

  ASSERT (pg->location != NULL);

  struct swap_slot *s = (struct swap_slot *) pg->location;

  /* Page and slot must agree. */
  ASSERT (pg == s->pg);

  /* Wipe the slot. */
  s->pg = NULL;

  /* Toggle bitmap status. */
  lock_acquire (&system_swap_table.usage_lock);
  ASSERT (bitmap_test (system_swap_table.usage, s->id) == 1);
  bitmap_flip (system_swap_table.usage, s->id);
  lock_release (&system_swap_table.usage_lock);

  /* Update page info. */
  pg->status = PAGE_DISCARDED; 
  pg->location = NULL;
}
