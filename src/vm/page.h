#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <list.h>
#include <hash.h>

struct supp_page_table;
struct page;/*changed from mapping*/

enum page_state
{
  PAGE_RESIDENT, /* This page is in a frame. */
  PAGE_SWAPPED_OUT, /* This page is in a swap slot. */
  PAGE_IN_FILE, /* This page is in a file (mmap). */
  PAGE_NEVER_ACCESSED /* This page has never been accessed. */
};

enum segment_type
{
  SEGMENT_PRIVATE, /* Not shared between multiple processes. */
  SEGMENT_SHARED_RO /* RO shared segment. */
}

/* Structure tracking the mapping between a virtual address (page) and its location. */
/* TODO should this be called a page? *//*made changes
struct page
{
  /* OWNER */
  struct supp_page_table *spt; /* Supp. Page Table that contains this mapping. */
  int32_t segment_page; /* Which page in its segment is this? */

  void *paddr; /* Corresponding physical address (OR swap address?). Set by Frame Table. */

  /* TODO Do we need either or both? */
  frame_id_t fid; /* ID of the frame/slot in which this page resides. */
  unsigned stamp; /* Stamp of the frame/slot in which this page resides. */

  /* JUST USE A STRUCT FILE * */
  /* If mmap'd, this is the fd for the backing file. */
  int fd;
  bool is_mmap;
  mmap_id_t mmap_id; /* TODO Not sure if we need this. */
  /* TODO track whether or not this is the rw portion of the executable: if so, 
     load from file but then transition to a "stack page" (i.e. don't use file
     as backing store, but don't discard the data, either). */

  enum page_state state; /* Where is the data associated with this page? */
  bool is_pinned; /* Whether or not this page is pinned. */

  struct lock mapping_lock; /* TODO: Is this how SPT and FT should communicate w.r.t. eviction? */

  struct hash_elem elem; /* For inclusion in the hash of a struct segment. Hash on segment_page. */
  struct list_elem fte_elem; /* TODO Need this? For inclusion in the list of a frame table entry. */
};

/* Mappings that may be shared between multiple processes. 
   Each process maintains its own concept of what the page number means. */
struct shared_mappings
{
  struct lock lock; /* For atomic update of the mappings. */
  struct hash *page_mappings; /* Maps from page number to mapping. */
};

/* Container for a segment shared between multiple processes.
 
   Allocated by the first process to access the shared segment.
   Cleaned up by the last process to release the shared segment. 
   
   A pointer to the page_mappings element can be cast to the ro_shared_segment itself. */
struct ro_shared_segment
{
  struct shared_mappings page_mappings;
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

  struct shared_mappings *page_mappings; /* Maps from segment page number to mapping. Can be shared with other processes. */
  /* SEGMENT_PRIVATE: We are the only one using page_mappings.
     SEGMENT_SHARED_RO: page_mappings is shared. Need to acquire lock before 
       defining new mappings. Can be cast to a 'struct ro_shared_segment *' if needed. */
  enum segment_type type; 
  
  struct list_elem elem; /* For inclusion in the segment list of a struct supp_page_table. */
}

/* Maps all virtual addresses known to this process. */
struct supp_page_table
{
  struct list segment_list; /* List of segments, sorted by their starting addresses. (Stack segment is always last, since it is contiguous and ends just below PHYS_BASE). */
};

#endif /* vm/page.h */
