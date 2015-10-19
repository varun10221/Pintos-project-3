#ifndef VM_PAGE_H 
#define VM_PAGE_H

#include <list.h>
#include <hash.h>
#include <stdint.h>

#include "threads/synch.h"
#include "devices/block.h"
#include "filesys/off_t.h"

/* TODO Move the "implementation-level" declarations to page.c. Prefer an opaque interface where possible. */

/* mmap flags */

/* Sharing between processes. */
static const int MAP_PRIVATE = 1 << 0;
static const int MAP_SHARED  = 1 << 1;

/* Access info for the segment holding the pages. */
static const int MAP_RDONLY  = 1 << 2;
static const int MAP_RDWR    = 1 << 3;

enum page_status
{
  PAGE_RESIDENT, /* This page is in a frame. */
  PAGE_SWAPPED_OUT, /* This page is in a swap slot. */
  PAGE_IN_FILE, /* This page is in a file (mmap). */
  PAGE_STACK_NEVER_ACCESSED, /* This stack page has never been accessed. Zero it out before storing in frame please. */
  PAGE_DISCARDED /* This page has been discarded by its owner and is going to be deleted. */
};

enum segment_type
{
  SEGMENT_PRIVATE, /* Not shared between multiple processes. */
  SEGMENT_SHARED /* RO shared segment. */
};

enum mmap_backing_type
{
  MMAP_BACKING_PERMANENT, /* mmap is backed by a file forever. */
  MMAP_BACKING_INITIAL    /* mmap is backed by a file temporarily: once read, it becomes swap'able. */
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
  struct list segment_list; /* List of segments, sorted by their starting addresses. (Stack segment is always last, since it is contiguous and ends at PHYS_BASE). */
};

struct mmap_details
{
  struct file *mmap_file; /* If not NULL, segment is backed by this file. */
  off_t offset; /* mmap is based on this offset in the file. */

  enum mmap_backing_type backing_type;
  uint32_t read_bytes; /* Used for MMAP_BACKING_INITIAL. The first READ_BYTES of the executable are read. */
  uint32_t zero_bytes; /* Used for MMAP_BACKING_INITIAL. Bytes after READ_BYTES are zero'd. */
};

/* Info needed to:
     - map 'relative page numbers' to 'pages'
     - evict and replace such pages. 
   A segment points to either one of these or to a struct shared_mappings*. */
struct segment_mapping_info
{
  struct hash mappings; /* Maps relative page number to page. */
  struct mmap_details mmap_details; /* Info about whether or not this is an mmap'd segment. */
  int flags;
};

/* Tracks a particular address range for a process. */
struct segment
{
  /* An address belongs to this segment if (start <= addr && addr < end). */
  void *start; /* Starting address of this segment (virtual address). Might not be page-aligned (e.g. stack segment).*/
  void *end; /* One byte past the final address in this segment (virtual address). Might not be page-aligned (e.g. mmap). */

  /* Allows us to map from segment page number to page. 
     Points to either a struct segment_mapping_info* or to a struct shared_mappings*. 
     The pointer type is determined by segment.type. */
  void *mappings; 
  /* SEGMENT_PRIVATE: We are the only one using mappings. mappings points to a struct segment_mapping_info*
     SEGMENT_SHARED: We share mappings. mappings points to a struct shared_mappings*. */
  enum segment_type type; 

  struct list_elem elem; /* For inclusion in the segment list of a struct supp_page_table. */
};

/* Container for mappings shared between multiple processes.
   Only supports mappings for mmap'd files, but I think it should
   generalize pretty easily if need be.
 
   Allocated by the first process to access the shared segment.
   Cleaned up by the last process to release the shared segment. */
struct shared_mappings
{
  /* Sector of the inode to which the file is mapped; unique in the FS across 
       the lifetime of the executable. Used as the hash field in ro_shared_mappings_table. */
  block_sector_t inumber; 

  struct segment_mapping_info smi; /* All processes share this mapping info. */ 
  struct lock segment_mapping_info_lock; /* Protects smi. Must acquire this to add or remove pages from this mapping. */

  int ref_count; /* How many processes are using this shared segment? Last one done has to clean up. */
  struct lock ref_count_lock; /* Protects ref_count. */

  struct hash_elem elem; /* For storage in the ro_segment_table. */ 
};

/* A single global structure is defined to allow the sharing of the RO pages of executables. */
struct ro_shared_mappings_table
{
  struct hash inumber_to_segment; /* Hash inode number to a shared_mappings. */
  struct lock hash_lock; /* Lock before modifying the hash. */
};

/* A page tracks a list of its "owners": processes that need to be notified
   if the page's location changes. 
   A page_owner_info contains enough information to update the pagedir for
   each owning process. */
struct page_owner_info
{
  struct thread *owner; /* This thread has a reference to this page. */
  void *vpg_addr; /* This is the virtual page address in the owner's address space. */
  bool writable; /* Whether or not this mapping is writable. */
  struct list_elem elem;
};

/* Structure tracking the mapping between a virtual address (page) and its location. */
struct page
{
  /* List of page_owner_info's of the processes that use this page. 
     NB The pagedirs (HW page tables) of the owners are updated and invalidated
     by the frame table. The SPT itself should not modify the pagedir at all. 

     Use lock to control access to this field. */
  struct list owners; 
  bool is_dirty; /* If an owner with a dirty pagedir entry for this page calls page_remove_owner, he sets this to true. */ 

  void *location; /* If page is in frame or swap table, this is the struct frame* or struct slot* in which this page resides. */
  enum page_status status; /* Status of this page. */

  struct segment_mapping_info *smi; /* Info for mmap and knowledge about rw status. */
  int32_t segment_page; /* Which page in its segment is this? Segments hash pages by segment_page. */

  struct lock lock; /* Used to protect owners and location+status. */
  struct hash_elem elem; /* For inclusion in the hash of a struct segment. Hash on segment_page. */
  struct list_elem dead_elem; /* For adding to a list of dead pages in segment_destroy. A page must be present in exactly one segment_mapping_info's mappings hash. */
};

/* Tracks the segment* associated with a given mapid_t. 
   Stored in a thread's mmap_table.
   The segment tracks the corresponding file* for loading data into a frame.
   On munmap we use this to find the segment* whose pages need to be handled. */
struct mmap_info
{
  struct segment *seg; /* The segment* corresponding to this mapping. */
};

/* Read-only shared segment table: Basic life cycle. */
void ro_shared_mappings_table_init (void);
void ro_shared_mappings_table_destroy (void);

struct shared_mappings * ro_shared_mappings_table_get (struct file *, int);
void ro_shared_mappings_table_remove (struct file *);

/* Supplemental page table: Basic life cycle. */
void supp_page_table_init (struct supp_page_table *);
void supp_page_table_destroy (struct supp_page_table *);

/* Usage. */
struct page * supp_page_table_find_page (struct supp_page_table *, void *vaddr);
struct segment * supp_page_table_add_mapping (struct supp_page_table *, struct mmap_details *md, void *, int, bool);
void supp_page_table_remove_segment (struct supp_page_table *, struct segment *);

/* Some page APIs for use by frame. */
void page_clear_owners_pagedir (struct page *);
void page_update_owners_pagedir (struct page *, void *);
bool page_unset_dirty (struct page *);
bool page_check_accessbit_decide_eviction_pagedir (struct page * , struct frame *);

#endif /* vm/page.h */
