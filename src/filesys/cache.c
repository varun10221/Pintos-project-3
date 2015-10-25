#include "cache.h"

#include "threads/thread.h"
#include "threads/synch.h"
#include "devices/timer.h"
#include "filesys/filesys.h"

enum cache_block_type
{
  CACHE_BLOCK_DATA,
  CACHE_BLOCK_METADATA
};

/* Entry in buffer_cache_table. In-memory cached version of on-disk data. */
struct cache_block
{
  uint32_t cache_ix;            /* Index in the cache. */
  block_sector_t block;         /* Block address */
  enum cache_block_type type;         /* Data or metadata? */

  bool is_dirty;                /* If the block has been modified. */
  bool is_valid;                /* If the data is valid. */
  uint32_t readers_count;       /* No. of readers reading the block. */
  bool is_writer;               /* Is there a writer present? */
  struct lock state_lock;       /* For atomic updates to the above fields. */

  /* TODO Synchronization mechanism to signal pending readers/writers. */
  uint32_t pending_requests;    /* Pending requests waiting due to read/write regulation. */

  /* TODO Usage information for eviction policy. */

  /* TODO Where is the data? */
};

/* The global buffer_cache_table stores cache_blocks. */
#define MAX_SECTORS_IN_CACHE 64
#define CACHE_SIZE (64 / DATA_BLOCKSIZE)
struct buffer_cache_table
{
  uint32_t n_free_cache_blocks; /* No. of free blocks. */
  uint32_t n_total_cache_blocks; /* Total no. of cache blocks in table. */
  struct cache_block blocks[CACHE_SIZE]; /* Array for cache_table, table utmost will hold 64 sector's worth of data */
  struct lock buffer_cache_lock; /* any atomic operations needs to be performed */
};

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

/* Initialize the buffer cache. */
void 
cache_init (void)
{

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
  struct semaphore *sema = (struct semaphore *) arg;

  sema_up (sema);

  /* TODO Implement this. */
  for (;;)
  {
    timer_msleep (1000*30);
  }
}

/* TODO Once this is "working", remove the filesys_lock() and filesys_unlock()
   from the rest of the code base. */
