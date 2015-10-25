#include "cache.h"

#include <string.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "devices/timer.h"
#include "devices/block.h"

/* Entry in a buffer_cache_table. In-memory cached version of on-disk data. */
struct cache_block
{
  uint32_t cache_ix;            /* Index in the cache. */
  block_sector_t block;         /* Block address */
  enum cache_block_type type;   /* Data or metadata? */

  bool is_dirty;                /* If the block has been modified. */
  bool is_valid;                /* If the data is valid. If true and if is_dirty, need to flush on eviction. */
  uint32_t readers_count;       /* No. of readers reading the block. */
  bool is_writer;               /* Is there a writer present? */
  struct lock state_lock;       /* For atomic updates to the above fields. */

  /* TODO Synchronization mechanism to signal pending readers/writers. */
  uint32_t pending_requests;    /* Pending requests waiting due to read/write regulation. */

  /* TODO Usage information for eviction policy. */

  void *contents;               /* Pointer to the contents of this block. */
};

/* A global buffer_cache_table stores cache_blocks. */
#define CACHE_SIZE 64
struct buffer_cache_table
{
  uint32_t n_free_cache_blocks; /* No. of free blocks. */
  uint32_t n_total_cache_blocks; /* Total no. of cache blocks in table. */
  struct cache_block blocks[CACHE_SIZE]; /* Array for cache_table, table utmost will hold 64 sector's worth of data */
  struct lock buffer_cache_lock; /* any atomic operations needs to be performed */
  struct block *backing_block; /* The block on which this cache is built. */
};

/* Elements of readahead_queue. */
typedef struct readahead_request_s
{
  block_sector_t address; /* What address to readahead? */
  enum cache_block_type type; /* What type to readahead? */
  struct list_elem elem; /* For inclusion in readahead_queue. */
} readahead_request_t;

/* Private variables used to implement the buffer cache. */
static struct buffer_cache_table buffer_cache;

/* Private functions. */

/* Buffer cache. */
static void buffer_cache_init (struct block *);
static void buffer_cache_destroy (void);

static void cache_evict_block (struct cache_block *);
static void cache_flush_block (struct cache_block *);
static void cache_mark_block_clean (struct cache_block *);

/* Cache block. */
static void cache_block_init (struct cache_block *);
static size_t cache_block_alloc_contents (struct cache_block *);
static size_t cache_block_get_contents_size (const struct cache_block *);
static size_t cache_block_get_n_sectors (const struct cache_block *cb);

/* Kernel threads. */
static void bdflush (void *arg);
static void readahead (void *arg);

/* Args passed to the bdflush thread. */
struct bdflush_args_s
{
  struct semaphore sema;
  int64_t flush_freq;
};

/* Readahead queue: A dynamically-sized queue of readahead requests.
 
   Items are enqueued by any thread that calls cache_readahead.
   Items are dequeued by the readahead thread.

   The lock is used for atomic access to the queue.
   The sema is Up'd at the end of cache_readahead, 
     and Down'd by the readahead thread (to wait for a new item). */
static struct list readahead_queue;
static struct lock readahead_queue_lock;
static struct semaphore readahead_queue_items_available;

/* Function definitions. */

/* Initialize the buffer cache module. */
void 
cache_init (struct block *backing_block)
{
  ASSERT (backing_block != NULL);
  buffer_cache_init (backing_block);

  /* Start kernel threads associated with the buffer cache. */
  tid_t bdflush_tid, readahead_tid;

  /* Create the bdflush thread. */
  struct bdflush_args_s bdflush_args;
  sema_init (&bdflush_args.sema, 0);
  bdflush_args.flush_freq = 30;
  bdflush_tid = thread_create ("bdflush", PRI_DEFAULT, bdflush, &bdflush_args);
  ASSERT (bdflush_tid != TID_ERROR);
  /* Wait for bdflush to get started (otherwise the bdflush_args pointer could be invalid). */
  sema_down (&bdflush_args.sema);

  /* Create the readahead thread. */
  /* Initialize the readahead queue used by the readahead thread. */
  list_init (&readahead_queue);
  lock_init (&readahead_queue_lock);
  sema_init (&readahead_queue_items_available, 0);

  struct semaphore readahead_started;
  sema_init (&readahead_started, 0);
  readahead_tid = thread_create ("readahead", PRI_DEFAULT, readahead, &readahead_started);
  ASSERT (readahead_tid != TID_ERROR);
  /* Wait for readahead to get started. */
  sema_down (&readahead_started);

}

/* Initialize the global buffer_cache_table struct. */
static void buffer_cache_init (struct block *backing_block)
{
  ASSERT (backing_block != NULL);
  buffer_cache.backing_block = backing_block;

  /* Initialize each block. */
  int i;
  for (i = 0; i < CACHE_SIZE; i++)
    cache_block_init (&buffer_cache.blocks[i]);
}

/* Destroy the global buffer cache, flushing any dirty blocks to disk. */
static void buffer_cache_destroy (void)
{
  /* TODO Signal to the kernel threads that we are done? */
}

/* Initialize this cache block. */
void cache_block_init (struct cache_block *cb)
{
  ASSERT (cb != NULL);
  memset (cb, 0, sizeof(struct cache_block));

  lock_init (&cb->state_lock);

  /* 0 readers and no writer: Not in use. */
  cb->readers_count = 0;
  cb->is_writer = 0;
}

/* Dynamically allocate the contents field of CB. 
   Requires cb->type to be set appropriately. 
   Returns the size of contents. */
static size_t 
cache_block_alloc_contents (struct cache_block *cb)
{
  ASSERT (cb != NULL);
  ASSERT (!cb->is_valid);
  ASSERT (cb->contents == NULL);

  size_t n_bytes = 0;
  switch (cb->type)
  {
    case CACHE_BLOCK_INODE:
      n_bytes = INODE_SIZE;
      break;
    case CACHE_BLOCK_DATA:
      n_bytes = DATA_BLOCKSIZE;
      break;
    case CACHE_BLOCK_METADATA:
      n_bytes = METADATA_BLOCKSIZE;
      break;
    default:
      NOT_REACHED ();
  }

  cb->contents = malloc (n_bytes);
  ASSERT (cb->contents != NULL);

  return n_bytes;
}

/* Evict the contents of CB. */
static void 
cache_block_evict (struct cache_block *cb)
{
  ASSERT (cb != NULL);
  ASSERT (cb->is_dirty);
}

/* Destroy the buffer cache. */
void 
cache_destroy (void)
{

}

/* Reserve a block in the buffer cache dedicated to hold this block.
   This may evict another buffer.
   Grants exclusive or shared access.
   If the block is already in the buffer cache, we just return it. */
struct cache_block * 
cache_get_block (block_sector_t address, enum cache_block_type type, bool exclusive)
{
  return NULL;
}

/* Release access to this cache block. */
void cache_put_block (struct cache_block *cb)
{
  ASSERT (cb != NULL);

  lock_acquire (&cb->state_lock);

  /* If I was the writer, I'm done. */
  if (cb->is_writer)
  {
    ASSERT (cb->readers_count == 0);
    cb->is_writer = false;
  }
  /* If I was a reader, I'm done. */
  else
  {
    ASSERT (0 < cb->readers_count);
    cb->readers_count--;
  }

  lock_release (&cb->state_lock);
}

/* Fill CB with data and return pointer to the data. 
   CB->TYPE and CB->BLOCK must be set correctly. */
void * cache_read_block (struct cache_block *cb)
{
  ASSERT (cb != NULL);

  size_t i;
  size_t n_bytes = cache_block_alloc_contents (cb);
  size_t n_sectors = n_bytes / BLOCK_SECTOR_SIZE;

  for (i = 0; i < n_sectors; i++)
  {
    size_t rel_offset = i*BLOCK_SECTOR_SIZE;
    block_read (buffer_cache.backing_block, cb->block + rel_offset, cb->contents + rel_offset);
  }

  cb->is_valid = true;

  return cb->contents;
}

/* Fill CB with zeros and return pointer to the data. 
   CB->TYPE must be set correctly.
   CB is marked dirty. */
void * cache_zero_block (struct cache_block *cb)
{
  ASSERT (cb != NULL);

  /* Allocate and zero out contents. */
  size_t n_bytes = cache_block_alloc_contents (cb);
  ASSERT (cb->contents != NULL);
  memset (cb->contents, 0, n_bytes);

  cb->is_dirty = true;
  cb->is_valid = true;

  return cb->contents;
}

/* Flush all dirty pages from the cache. */
void 
cache_flush (void)
{
  int i;
  for (i = 0; i < CACHE_SIZE; i++)
  {

  }
}

/* Mark CB dirty. */
void cache_mark_block_dirty (struct cache_block *cb)
{
  ASSERT (cb != NULL);
  ASSERT (cb->is_valid);

  cb->is_dirty = true;
}

/* Request that ADDRESS of TYPE be cached. It will be cached with
   no readers or writers.

   This request may be fulfilled by the calling process before the 
   readahead thread handles it. */
void cache_readahead (block_sector_t address, enum cache_block_type type)
{
  readahead_request_t *req = (readahead_request_t *) malloc (sizeof(readahead_request_t));
  if (req != NULL)
  {
    req->address = address;
    req->type = type;
    /* Safely add to list. */
    lock_acquire (&readahead_queue_lock);
    list_push_back (&readahead_queue, &req->elem)
    lock_release (&readahead_queue_lock);
    /* Signal the readahead thread. */
    sema_up (&readahead_queue_items_available);
  }
}

/* bdflush kernel thread. 
   ARG is a 'struct bdflush_args_s'.
   Flushes dirty pages from buffer cache to disk every FLUSH_FREQ seconds. */
static void 
bdflush (void *arg)
{
  /* Extract args. */
  ASSERT (arg != NULL);
  struct bdflush_args_s *bdflush_args = (struct bdflush_args_s *) arg;
  int64_t freq = bdflush_args->flush_freq;
  ASSERT (0 < freq);

  sema_up (&bdflush_args->sema);

  for (;;)
  {
    timer_msleep (1000*freq);
    cache_flush ();
  }
}

/* readahead kernel thread.
   We use the producer/consumer model to determine which sectors to read ahead. 
   This is the consumer. */
static void 
readahead (void *arg)
{
  ASSERT (arg != NULL);
  struct semaphore *initialized_sema = (struct semaphore *) arg;

  sema_up (initalized_sema);

  /* TODO Implement this. */
  for (;;)
  {
    sema_down (&readahead_queue_items_available);

    /* Get the readahead request. */
    lock_acquire (&readahead_queue_lock);
    ASSERT (!list_empty (&readahead_queue));
    struct readahead_request_s *req = list_entry (list_pop_front (&readahead_queue), struct readahead_request_s, elem);
    lock_release (&readahead_queue_lock);
    ASSERT (req != NULL);

    struct cache_block *cb = cache_get_block (address, type, false);
    ASSERT (cb != NULL);
    /* If the request hasn't been read yet by whomever we are reading ahead for,
       read it now. */
    if (!cb->is_valid)
      cache_read_block (cb);
    free (req);
  }

}

/* TODO Once this is "working", remove the filesys_lock() and filesys_unlock()
   from the rest of the code base. */
