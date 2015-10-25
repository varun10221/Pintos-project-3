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

/* Readahead queue: A dynamically-sized queue of readahead requests.
 
   Items are enqueued by any thread that calls cache_readahead.
   Items are dequeued by the readahead thread.

   The lock is used for atomic access to the queue.
   The sema is Up'd at the end of cache_readahead, 
     and Down'd by the readahead thread (to wait for a new item). */
static struct list readahead_queue;
static struct lock readahead_queue_lock;
static struct sema readahead_queue_items_available;

/* Private functions. */

/* Buffer cache. */
void buffer_cache_table_init (size_t);
void buffer_cache_table_destroy (void);

/* Kernel threads. */
static void bdflush (void *arg);
static void readahead (void *arg);

struct bdflush_args_s
{
  struct semaphore sema;
  int64_t flush_freq;
};

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

/* Destroy the buffer cache. */
void 
cache_destroy (void)
{

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

/* Put an entry for ADDRESS into the buffer cache. */
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
