#include "vm/page.h"

#include <debug.h>

struct ro_shared_segment_table ro_shared_segment_table;

/* Initialize the ro shared segment table. Not thread safe. Should be called once. */
void 
ro_shared_segment_table_init (void)
{
  /* TODO */
}

/* Destroy the ro shared segment table. Not thread safe. Should be called once. */
void 
ro_shared_segment_table_destroy (void)
{
  /* TODO */
}

/* Initialize a supplemental page table.
   This SPT begins with only a stack segment. */
void supp_page_table_init (struct supp_page_table *spt)
{
  ASSERT (spt != NULL);
  /* TODO */
}

/* Destroy this supplemental page table, releasing
   all resources it holds. */
void supp_page_table_destroy (struct supp_page_table *spt)
{
  ASSERT (spt != NULL);
  /* TODO */
}

/* Return the page associated with virtual address VADDR.
   If VADDR looks like a stack growth, grow the stack and add a page.

   Returns NULL if no such page (i.e. illegal memory access). */
struct page * supp_page_table_find_page (struct supp_page_table *spt, void *vaddr)
{
  ASSERT (spt != NULL);
  ASSERT (vaddr != NULL);

  /* TODO */
  return NULL;
}

/* Add a {read_only | writable} mapping for mapid MAPID starting at address VADDR.

   MAPID must refer to a valid mapping.
 
   Returns true on success, false if VADDR is 0, is not page-aligned, or overlaps
   any already-mapped pages. */

/* Add a memory mapping to supp page table SPT
     for file F beginning at START with flags FLAGS.
   Returns NULL on failure.

   Caller should NOT free the segment.
   Use supp_page_table_remove_segment() to clean up the segment. */
struct segment * supp_page_table_add_mapping (struct supp_page_table *spt, struct file *f, void *vaddr, int flags)
{
  ASSERT (spt != NULL);
  ASSERT (f != NULL);
  ASSERT (vaddr != NULL);

  /* TODO */
  ASSERT (0 == 1);
}

/* Remove the specified segment from this page table.
   If it's an mmap'd segment, will flush all dirty pages. 
   Will free the memory associated with this segment
   If it's a shared mapping, and if we're the last holder of the 
     pages, will free that memory too. 
   Don't forget to close the file! */
void supp_page_table_remove_segment (struct supp_page_table *spt, struct segment *seg)
{
  ASSERT (spt != NULL);
  ASSERT (seg != NULL);

  /* TODO */
  ASSERT (0 != 1);
}


/*
  QUESTION FROM VARUN:
  how can we add and delete info about individual pages ?
  say if a page is evicted out of a frame.
  API's for that 
*/
