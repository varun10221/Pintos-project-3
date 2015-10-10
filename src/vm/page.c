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
struct page * supp_page_table_find_page (void *vaddr)
{
  ASSERT (vaddr != NULL);
  return NULL;
}

/* Add a {read_only | writable} mapping for mapid MAPID starting at address VADDR.

   MAPID must refer to a valid mapping.
 
   Returns true on success, false if VADDR is 0, is not page-aligned, or overlaps
   any already-mapped pages. */
bool supp_page_table_add_mapping (mapid_t mapid, void *vaddr, bool is_writable)
{
  ASSERT (0 <= mapid);
  ASSERT (vaddr != NULL);

  return false;
}

/* Remove the mapping for mapid MAPID. 
   All dirtied pages are written back to the file.
 
   MAPID must refer to a valid mapping. */
void supp_page_table_remove_mapping (mapid_t mapid)
{
  ASSERT (0 <= mapid);
}
