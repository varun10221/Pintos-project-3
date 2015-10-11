#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <list.h>
#include <hash.h>
#include <stdint.h>

#include "threads/synch.h"
#include "devices/block.h"

enum page_status
{
  PAGE_RESIDENT, /* This page is in a frame. */
  PAGE_SWAPPED_OUT, /* This page is in a swap slot. */
  PAGE_IN_FILE, /* This page is in a file (mmap). */
  PAGE_NEVER_ACCESSED, /* This page has never been accessed. */
  PAGE_DISCARDED /* This page has been discarded by its owner and is going to be deleted. */
};

enum segment_type
{
  SEGMENT_PRIVATE, /* Not shared between multiple processes. */
  SEGMENT_SHARED_RO /* RO shared segment. */
};

enum popularity_range

{
  POPULARITY_MIN = 0,  /* Lowest value a page's popularity can take on. */
  POPULARITY_START = 127, /* Initial value for a new page's popularity. */
  POPULARITY_MAX = 255 /* Highest value a page's popularity can take on. */
};

/* Maps all virtual addresses known to this process. */
struct supp_page_table
{
  struct list segment_list; /* List of segments, sorted by their starting addresses. (Stack segment is always last, since it is contiguous and ends just below PHYS_BASE). */
};

/* Pages that may be shared between multiple processes. 
   Each process maintains its own concept of what the page number means. */
struct shared_pages
{
  struct lock lock; /* For atomic update of the mappings. */
  struct hash *page_mappings; /* Maps from page number to page. */
};

/* Container for a segment shared between multiple processes.
 
   Allocated by the first process to access the shared segment.
   Cleaned up by the last process to release the shared segment. 
   
   A pointer to the page_mappings element can be cast to the ro_shared_segment itself. */
struct ro_shared_segment
{
  struct shared_pages page_mappings; /* Maps relative page number to page. */
  block_sector_t inode_sector; /* Sector of the inode to which the file is mapped; unique in the FS across the lifetime of the executable. */

  int ref_count; /* How many processes are using this shared segment? Last one done has to clean up. */
  struct lock ref_count_lock; /* Atomic modifications. */

  struct hash_elem elem; /* For storage in the ro_segment_table. Hash on inode_start. */
};

/* A single global structure is defined to allow the sharing of the RO pages of executables. */
struct ro_shared_segment_table
{
  struct hash inode_to_segment; /* Hash inode number to an ro_shared_segment. */
  struct lock hash_lock; /* Lock before modifying the hash. */
};

/* Tracks a particular address range for a process. */
struct segment
{
  /* An address belongs to this segment if (start <= addr && addr < end). */
  void *start; /* Starting address of this segment (virtual address). */
  void *end; /* One address past the final address of this segment (virtual address). */

  struct shared_pages *page_mappings; /* Maps from segment page number to page. Can be shared with other processes. */

  /* SEGMENT_PRIVATE: We are the only one using page_mappings.
     SEGMENT_SHARED_RO: page_mappings is shared. Need to acquire lock before 
       defining new mappings. Can be cast to a 'struct ro_shared_segment *' if needed. */
  enum segment_type type; 
  
  struct list_elem elem; /* For inclusion in the segment list of a struct supp_page_table. */
};

/* Tracks the file* and segment* associated with a given mapid_t. 
   mmap_info's are stored in a thread's mmap_table so that on mmap we have the file*
   and munmap we have the segment*. */
struct mmap_info
{
  struct file *f; /* A file* for I/O to the file mapped in our address space. */
  struct segment *seg; /* The segment* corresponding to this mapping. */
};

/* A page tracks a list of its "owners": processes that need to be notified
   if the page's location changes. 
   A page_owner_info contains enough information to update the pagedir for
   each owning process. */
struct page_owner_info
{
  struct thread *owner;
  void *vaddr;
  struct list_elem elem;
};

/* Structure tracking the mapping between a virtual address (page) and its location. */
struct page
{
  /* List of page_owner_info's of the processes that use this page. 
     NB The pagedirs (HW page tables) of the owners are updated and invalidated
     by the frame table. The SPT itself should not modify the pagedir at all. */
  struct list owners; 

  int32_t segment_page; /* Which page in its segment is this? */

  void *location; /* struct frame* or struct slot* in which this page resides. */
  unsigned stamp; /* Stamp of the frame/slot in which this page resides. For ABA problem. TODO Do we need this? */

  enum page_status status; /* Status of this page. */

  struct file *mmap_file; /* For loading and evicting pages in PAGE_IN_FILE state. */

  struct lock mapping_lock; /* TODO Is this how SPT and FT should communicate w.r.t. eviction? */
  struct hash_elem elem; /* For inclusion in the hash of a struct segment. Hash on segment_page. */
};

void ro_shared_segment_table_init (void);
void ro_shared_segment_table_destroy (void);

/* Basic life cycle. */
void supp_page_table_init (struct supp_page_table *);
void supp_page_table_destroy (struct supp_page_table *);

/* Usage. */
struct page * supp_page_table_find_page (void *vaddr);

bool supp_page_table_add_mapping (mapid_t, void *vaddr, bool is_writable);
void supp_page_table_remove_mapping (mapid_t);

#endif /* vm/page.h */
