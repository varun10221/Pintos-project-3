#include "cache.h"

#include <string.h>

#ifdef CACHE_DEBUG
#include <stdio.h>
#include <random.h>
#endif

#include <hash.h>

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
  block_sector_t block;         /* Block address */
  enum cache_block_type type;   /* Data or metadata? */

  /* Acquire to modify this cache_block. Used as the "monitor lock" for the condition variables below. */
  struct lock lock; 

  bool is_dirty;                /* If the block has been modified. */

  /* Fields to handle a mix of readers and writers.
   
     To avoid starvation, we assign equal weight to readers and writers. 
     Any time a process wishes to use the block but finds that the block is being used in a conflicting fashion, 
       it waits on the starving_process condition if there is no other waiter on that condition.
     If a process comes to use the block and finds a waiter already on the starving_process condition, it waits on the polite_processes
       condition instead of seeing if it can use the block immediately. 
       Thus, the presence of a waiter on the starving_process condition is used to guide threads
       into the "lower-priority" polite_processes waiting list.
     When the cache_block changes state (the last reader finishes, or the writer finishes):
       - if there is a process waiting on starving_process, it signals that condition.
       - else it broadcasts on the polite_processes condition variable, and lets those waiters "fight it out".

     If the block is evicted, the is_being_evicted bit is set to true. Upon waking a process tests this bit
       to see whether or not it should use this block.

     To evict a block: Remove the block from the hash (so no new processes can use it), then wait until 
       there are no active users and no pending users. Then modify block as desired.

     There are inefficiencies in this approach, but it should prevent starvation and provide some fairness. */

  uint32_t readers_count;       /* No. of readers reading the block. */
  bool is_writer;               /* Is there a writer present? */
  bool is_valid;                /* Is contents valid? */ 

  struct condition starving_process;  /* A process that would like to use the block, but cannot due to a conflicting use. */
  struct condition polite_processes;  /* If there is a waiter on the starving_process condition, other processes queue up on this condition variable to give the starving process a turn. */

  void *contents;               /* Pointer to the contents of this block. */

  struct list_elem l_elem; /* For inclusion in one of the buffer_cache_table's lists. */
  struct hash_elem h_elem; /* For inclusion in the buffer_cache_table's lookup hash. */
};

/* A global buffer_cache_table stores cache_blocks. */
#define CACHE_SIZE 64
struct buffer_cache_table
{
  struct list free_blocks;               /* List of free cache blocks. */
  struct list in_use_blocks;             /* List of in-use cache blocks, ordered with MRU at the head of the list. */
  struct list being_evicted_blocks;      /* List of blocks that are being evicted. */
  struct condition no_blocks_to_evict;   /* If all CACHE_SIZE blocks are being evicted at once, wait on this until signaled. */

  struct cache_block blocks[CACHE_SIZE]; /* The cache_blocks we use. */
  struct hash addr_to_block;             /* Maps address to cache_block. */


  struct lock lock;                      /* Serializes access to struct members. */

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
static struct cache_block * buffer_cache_get_eviction_victim (void);

static void cache_evict_block (struct cache_block *);
static void cache_flush_block (struct cache_block *);
static void cache_mark_block_clean (struct cache_block *);

/* Cache block. */
static void cache_block_init (struct cache_block *);
static void cache_block_get_access (struct cache_block *, bool);
static size_t cache_block_alloc_contents (struct cache_block *);
static void cache_block_destroy (struct cache_block *);
static void cache_block_set_hash (struct cache_block *, block_sector_t, enum cache_block_type);
static unsigned cache_block_hash_func (const struct hash_elem *, void *);
static bool cache_block_less_func (const struct hash_elem *, const struct hash_elem *, void *);
static size_t cache_block_get_contents_size (const struct cache_block *);
static size_t cache_block_get_n_sectors (const struct cache_block *cb);

/* Testing. */
#ifdef CACHE_DEBUG
static void cache_self_test_thread (void *);
static void cache_self_test_single (void);
static void cache_self_test_parallel (void);
#endif
  

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

  /* Create the readahead thread. */
  /* Initialize the readahead queue used by the readahead thread. */
  list_init (&readahead_queue);
  lock_init (&readahead_queue_lock);
  sema_init (&readahead_queue_items_available, 0);

  struct semaphore readahead_started;
  sema_init (&readahead_started, 0);
  readahead_tid = thread_create ("readahead", PRI_DEFAULT, readahead, &readahead_started);
  ASSERT (readahead_tid != TID_ERROR);

  /* Wait for bdflush to get started (otherwise the bdflush_args pointer could be invalid). */
  sema_down (&bdflush_args.sema);
  /* Wait for readahead to get started. */
  sema_down (&readahead_started);

#ifdef CACHE_DEBUG
  cache_self_test_single ();
  cache_self_test_parallel ();
#endif
}

/* Destroy the buffer cache. */
void 
cache_destroy (void)
{
  /* TODO */
  buffer_cache_destroy ();
}


/* Initialize the global buffer_cache_table struct. */
static void 
buffer_cache_init (struct block *backing_block)
{
  ASSERT (backing_block != NULL);
  buffer_cache.backing_block = backing_block;

  lock_init (&buffer_cache.lock);
  list_init (&buffer_cache.free_blocks);
  list_init (&buffer_cache.in_use_blocks);
  list_init (&buffer_cache.being_evicted_blocks);
  cond_init (&buffer_cache.no_blocks_to_evict);
  hash_init (&buffer_cache.addr_to_block, cache_block_hash_func, cache_block_less_func, NULL);

  /* Initialize each cache block. */
  int i;
  for (i = 0; i < CACHE_SIZE; i++)
  {
    struct cache_block *cb = &buffer_cache.blocks[i];
    cache_block_init (cb);
    list_push_back (&buffer_cache.free_blocks, &cb->l_elem);
  }
}

/* Destroy the global buffer cache, flushing any dirty blocks to disk. */
static void 
buffer_cache_destroy (void)
{
  /* TODO Signal to the kernel threads that we are done? */
  /* TODO Clean up internals (at minimum, need to hash_destroy ()). */
}

/* Identify a cache block for eviction. 
   Must hold lock on buffer cache. 
   Places the victim cache block in the being_evicted_blocks list. */
static struct cache_block * 
buffer_cache_get_eviction_victim (void)
{
  ASSERT (lock_held_by_current_thread (&buffer_cache.lock));

#ifdef CACHE_DEBUG
  ASSERT (list_size (&buffer_cache.free_blocks) + list_size (&buffer_cache.in_use_blocks) + list_size (&buffer_cache.being_evicted_blocks) == CACHE_SIZE);
#endif

  /* We must need to evict in order to get a block. */
  ASSERT (list_empty (&buffer_cache.free_blocks));
  /* Wait until there is a block to evict. */
  while (list_empty (&buffer_cache.in_use_blocks))
    cond_wait (&buffer_cache.no_blocks_to_evict, &buffer_cache.lock);
  /* in_use_blocks is in MRU order, with MRU at front and LRU at back. */
  struct list_elem *e = list_pop_back (&buffer_cache.in_use_blocks);
  struct cache_block *cb = list_entry (e, struct cache_block, l_elem);

  list_push_front (&buffer_cache.being_evicted_blocks, &cb->l_elem);
  return cb;
}

/* Initialize this cache block. */
static void 
cache_block_init (struct cache_block *cb)
{
  ASSERT (cb != NULL);
  memset (cb, 0, sizeof(struct cache_block));

  lock_init (&cb->lock);
  cond_init (&cb->starving_process);
  cond_init (&cb->polite_processes);

  cb->is_valid = false;
  cb->contents = NULL;
  /* 0 readers and no writer: Not in use. */
  cb->readers_count = 0;
  cb->is_writer = false;
}

/* Obtain the appropriate access to locked CB.
   Waits until CB is available.
   Caller must hold the lock on CB. */
static void 
cache_block_get_access (struct cache_block *cb, bool exclusive)
{
  ASSERT (cb != NULL);
  ASSERT (lock_held_by_current_thread (&cb->lock));

  while (0 < cond_n_waiters (&cb->starving_process, &cb->lock))
    /* While there is a starving process, wait politely. */
    cond_wait (&cb->polite_processes, &cb->lock);
  ASSERT (cond_n_waiters (&cb->starving_process, &cb->lock) == 0);

  /* No starving process. Graduate to being the starving process if conflict. */
  bool is_conflict = (exclusive && 0 < cb->readers_count) || (!exclusive && cb->is_writer);
  while (is_conflict)
  {
    /* While there is a conflict, wait as the starving process. 
       Since only one process can ever become the starving process at a time,
         and since we only cond_signal() the starving_process condition when the user type can change,
         when I am woken there should no longer be a conflict. */
    cond_wait (&cb->starving_process, &cb->lock);
    is_conflict = (exclusive && 0 < cb->readers_count) || (!exclusive && cb->is_writer);
    ASSERT (!is_conflict);
  }

  /* There must now be no conflict between my intended usage and the block status. */
  is_conflict = (exclusive && 0 < cb->readers_count) || (!exclusive && cb->is_writer);
  ASSERT (!is_conflict);
  if (exclusive)
    cb->is_writer = true;
  else
    cb->readers_count++;
}

/* Destroy this cache block. */
static void 
cache_block_destroy (struct cache_block *cb)
{
  ASSERT (cb != NULL);
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

  size_t n_bytes = cache_block_get_contents_size (cb);
  cb->contents = malloc (n_bytes);
  ASSERT (cb->contents != NULL);

  return n_bytes;
}

/* Evict the contents of CB so that another block can be stored in it. 
   Caller must hold lock on buffer_cache. 
   Once eviction is completed, removes CB from being_evicted_blocks. 
   On return, CB is not in any of the buffer_cache lists. */
static void 
cache_evict_block (struct cache_block *cb)
{
  ASSERT (cb != NULL);
  ASSERT (lock_held_by_current_thread (&buffer_cache.lock));

  lock_acquire (&cb->lock);

  /* Remove block from the cache's addr table so no new processes
     will be able to find it. Now the usage will only decrease
     until we are the last with a reference to it. */
  hash_delete (&buffer_cache.addr_to_block, &cb->h_elem);   

  /* Wait until all other users are gone. */
  while (cb->is_writer || cb->readers_count || 
         0 < cond_n_waiters (&cb->starving_process, &cb->lock) || 
         0 < cond_n_waiters (&cb->polite_processes, &cb->lock))
    cond_wait (&cb->polite_processes, &cb->lock);

  /* Evict. */
  if (cb->is_dirty)
    cache_flush_block (cb);
  free (cb->contents);
  cb->contents = NULL;
  cb->is_valid = false;

  /* Remove from being_evicted_blocks list. */
  list_remove (&cb->l_elem);

  lock_release (&cb->lock);
}

/* Flush this dirty locked CB to disk. */
static void 
cache_flush_block (struct cache_block *cb)
{
  ASSERT (cb != NULL);
  ASSERT (lock_held_by_current_thread (&cb->lock));
  ASSERT (cb->is_dirty);

  size_t n_sectors = cache_block_get_n_sectors (cb);
  size_t i;
  for (i = 0; i < n_sectors; i++)
  {
    size_t rel_offset = i*BLOCK_SECTOR_SIZE;
    block_write (buffer_cache.backing_block, cb->block + rel_offset, cb->contents + rel_offset);
  }

  cache_mark_block_clean (cb);
}

/* Reserve a block in the buffer cache dedicated to hold this block.
   This may evict another block.
   Grants exclusive or shared access.
   If the block is already in the buffer cache, we re-use it. */
struct cache_block * 
cache_get_block (block_sector_t address, enum cache_block_type type, bool exclusive)
{
  /* Prepare to search hash. */
  struct cache_block *cb = NULL;
  struct cache_block dummy;
  cache_block_set_hash (&dummy, address, type);

  lock_acquire (&buffer_cache.lock);
  /* Is this block already in the cache? */
  struct hash_elem *match = hash_find (&buffer_cache.addr_to_block, &dummy.h_elem);
  if (match)
  {
    cb = hash_entry (match, struct cache_block, h_elem);
    /* Remove from current location in the in_use_blocks list. */
    list_remove (&cb->l_elem);
  }
  else
  {
    /* No match. Either get a free block or evict a block. */
    if (!list_empty (&buffer_cache.free_blocks))
      cb = list_entry (list_pop_front (&buffer_cache.free_blocks), struct cache_block, l_elem);
    else
    {
      cb = buffer_cache_get_eviction_victim ();
      cache_evict_block (cb);
    }

    cb->is_valid = false;
    cb->contents = NULL;
    /* Insert the new block into the hash so others can find it. */
    cb->block = address;
    cb->type = type;
    hash_insert (&buffer_cache.addr_to_block, &cb->h_elem);
  }

  ASSERT (cb != NULL);
  /* At this point, CB is not in any list. */

  /* This cache block is the most recently used, so put it at the front
     of the in_use_blocks list. */
  list_push_front (&buffer_cache.in_use_blocks, &cb->l_elem);

#ifdef CACHE_DEBUG
  ASSERT (list_size (&buffer_cache.free_blocks) + list_size (&buffer_cache.in_use_blocks) + list_size (&buffer_cache.being_evicted_blocks) == CACHE_SIZE);
#endif

  /* Lock CB so that we can release the buffer_cache lock
     without leaving a gap for eviction. */
  lock_acquire (&cb->lock);
  lock_release (&buffer_cache.lock);

  /* Update CB to indicate that we are using it. */
  cache_block_get_access (cb, exclusive);
  lock_release (&cb->lock);

  return cb;
}

/* Release access to this cache block. 
   starving_process and polite_processes are signaled appropriately.
   Even if no users, leave the block around -- only eviction will
   cause a block to leave the cache. */
void 
cache_put_block (struct cache_block *cb)
{
  ASSERT (cb != NULL);

  lock_acquire (&cb->lock);

  bool any_other_users = true;
  /* If there's a writer, that's me and I'm done. */
  if (cb->is_writer)
  {
    ASSERT (cb->readers_count == 0);
    cb->is_writer = false;
    any_other_users = false;
  }
  else if (0 < cb->readers_count)
  {
    ASSERT (!cb->is_writer);
    cb->readers_count--;
    if (cb->readers_count == 0)
      any_other_users = false;
  }
  else
    NOT_REACHED ();

  /* If we can change state, signal starving_process if there is one,
     otherwise broadcast to polite_processes. */
  if (!any_other_users)
  {
    if (cond_n_waiters (&cb->starving_process, &cb->lock))
      cond_signal (&cb->starving_process, &cb->lock);
    else if (cond_n_waiters (&cb->polite_processes, &cb->lock))
      cond_broadcast (&cb->polite_processes, &cb->lock);
  }

  lock_release (&cb->lock);
}

/* (If CB is not valid, fill CB with data and) return pointer to the data. 
   CB->TYPE and CB->BLOCK must be set correctly. */
void * 
cache_read_block (struct cache_block *cb)
{
  ASSERT (cb != NULL);
  size_t n_sectors, rel_offset, i;

  lock_acquire (&cb->lock);

  if (!cb->is_valid)
  {
    /* Not valid, so read from disk. 
       If already valid, don't read from disk -- could be dirty in which case we would
       drop the write. */
    cache_block_alloc_contents (cb);
    n_sectors = cache_block_get_n_sectors (cb);

    for (i = 0; i < n_sectors; i++)
    {
      rel_offset = i*BLOCK_SECTOR_SIZE;
      block_read (buffer_cache.backing_block, cb->block + rel_offset, cb->contents + rel_offset);
    }

    cb->is_valid = true;
  }

  lock_release (&cb->lock);
  return cb->contents;
}

/* (If CB is not valid, fill CB with zeros and) return pointer to the data. 
   CB->TYPE must be set correctly.
   CB is marked dirty. There must be a writer of CB. */
void * 
cache_zero_block (struct cache_block *cb)
{
  ASSERT (cb != NULL);

  lock_acquire (&cb->lock);
  ASSERT (cb->is_writer);

  if (!cb->is_valid)
  {
    /* Allocate and zero out contents. */
    size_t n_bytes = cache_block_alloc_contents (cb);
    ASSERT (cb->contents != NULL);
    memset (cb->contents, 0, n_bytes);

    cb->is_dirty = true;
    cb->is_valid = true;
  }

  lock_release (&cb->lock);
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

/* Mark CB dirty. There must be a writer. */
void 
cache_mark_block_dirty (struct cache_block *cb)
{
  ASSERT (cb != NULL);
  ASSERT (cb->is_valid);

  lock_acquire (&cb->lock);

  ASSERT (cb->is_writer);
  cb->is_dirty = true;

  lock_release (&cb->lock);
}

/* Internal function. Mark CB clean. Use with cache_flush_block. */
static void
cache_mark_block_clean (struct cache_block *cb)
{
  ASSERT (cb != NULL);
  ASSERT (cb->is_valid);

  cb->is_dirty = false;
}

/* Sets the fields of DUMMY appropriately. Keep in sync with cache_block_hash_func. */
static void 
cache_block_set_hash (struct cache_block *cb, block_sector_t address, enum cache_block_type type)
{
  ASSERT (cb != NULL);

  memset (cb, 0, sizeof(struct cache_block));
  cb->block = address;
  cb->type = type;
}


/* For use with buffer_cache.addr_to_block. 
   Hash is the hash of the CB's block field. 
   Keep in sync with cache_block_set_hash. */ 
static unsigned 
cache_block_hash_func (const struct hash_elem *cb_elem, void *aux UNUSED)
{
  ASSERT (cb_elem != NULL);
  struct cache_block *cb = hash_entry (cb_elem, struct cache_block, h_elem);
  ASSERT (cb != NULL);

  return hash_bytes (&cb->block, sizeof(block_sector_t));
}

/* For use with buffer_cache.addr_to_block. 
   Returns true if A < B, else false.
   Comparison is based on the block field of A and B.
   NULL is less than everything else. */
static bool 
cache_block_less_func (const struct hash_elem *a_elem, const struct hash_elem *b_elem, void *aux UNUSED)
{
  ASSERT (a_elem != NULL || b_elem != NULL);
  /* If a is NULL, a < b. */
  if (a_elem == NULL)
    return true;
  /* If b is NULL, b < a. */
  if (b_elem == NULL)
    return false;

  struct cache_block *a = hash_entry (a_elem, struct cache_block, h_elem);
  struct cache_block *b = hash_entry (b_elem, struct cache_block, h_elem);

  return (a->block < b->block);
}

/* Return the number of sectors spanned by CB->CONTENTS. */
static size_t
cache_block_get_n_sectors (const struct cache_block *cb)
{
  ASSERT (cb != NULL);

  return (cache_block_get_contents_size (cb) / BLOCK_SECTOR_SIZE);
}

/* Return the size in bytes of CB->CONTENTS. */
static size_t
cache_block_get_contents_size (const struct cache_block *cb)
{
  ASSERT (cb != NULL);

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

  return n_bytes;
}

/* Request that ADDRESS of TYPE be cached. It will be cached with
   no readers or writers.

   This request may be fulfilled by the calling process before the 
   readahead thread handles it. */
void 
cache_readahead (block_sector_t address, enum cache_block_type type)
{
  readahead_request_t *req = (readahead_request_t *) malloc (sizeof(readahead_request_t));
  if (req != NULL)
  {
    req->address = address;
    req->type = type;
    /* Safely add to list. */
    lock_acquire (&readahead_queue_lock);
    list_push_back (&readahead_queue, &req->elem);
    lock_release (&readahead_queue_lock);
    /* Signal the readahead thread. */
    sema_up (&readahead_queue_items_available);
  }
}

/* bdflush ("buffer dirty flush") kernel thread. 
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

  /* Signal caller that we're initialized. */
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

  /* Signal caller that we're initialized. */
  sema_up (initialized_sema);

  for (;;)
  {
    sema_down (&readahead_queue_items_available);

    /* Get the readahead request. */
    lock_acquire (&readahead_queue_lock);
    ASSERT (!list_empty (&readahead_queue));
    struct readahead_request_s *req = list_entry (list_pop_front (&readahead_queue), struct readahead_request_s, elem);
    lock_release (&readahead_queue_lock);
    ASSERT (req != NULL);

    /* Fulfill the request. */
    struct cache_block *cb = cache_get_block (req->address, req->type, false);
    ASSERT (cb != NULL);

    /* If the request hasn't been read yet by whomever we are reading ahead for,
       read it now. */
    if (!cb->is_valid)
      cache_read_block (cb);
    /* We are not using the block right now. Hopefully it is not evicted before 
       it is cache_get_block'd by whoever asked for it. */
    cache_put_block (cb);

    free (req);
  }

}

#ifdef CACHE_DEBUG
/* This is a test for the block cache. 
   Cache must have been cache_init()'d before calling this. 
   Hammers on the cache. 

   Client for cache_self_test_{single, parallel}. */
static void 
cache_self_test_thread (void *args)
{
  struct semaphore *is_done = (struct semaphore *) args;

  int i;
  struct cache_block *cb = NULL;

  /* Get CACHE_SIZE unique exclusive blocks. */
  for (i = 0; i < CACHE_SIZE; i++)
  {
    cb = cache_get_block (i, CACHE_BLOCK_INODE, true);
    cache_put_block (cb);
  }

  /* Get the same (new!) block 3*CACHE_SIZE times, shared. 
     The first get* should cause eviction. */
  for (i = 0; i < 3*CACHE_SIZE; i++)
  {
    cb = cache_get_block (CACHE_SIZE, CACHE_BLOCK_DATA, false);
    cache_put_block (cb);
  }

  cb = cache_get_block (i, CACHE_BLOCK_METADATA, true);
  char *data = cache_read_block (cb);
  cache_put_block (cb);

  /* Get a random block with random exclusive value. */
  for (i = 0; i < 100*CACHE_SIZE; i++)
  {
    int block = random_ulong () % (2*CACHE_SIZE);
    int exclusive = random_ulong () % 2;
    cb = cache_get_block (block, CACHE_BLOCK_INODE, exclusive ? true : false);
    cache_put_block (cb);
  }

  sema_up (is_done);
}

/* Single-threaded test of the cache. */
static void 
cache_self_test_single (void)
{
  struct semaphore is_done;
  sema_init (&is_done, 0);

  cache_self_test_thread (&is_done);
  sema_down (&is_done);
}

/* Multi-threaded test of the cache. */
static void
cache_self_test_parallel (void)
{
  int n_threads = 30;

  struct semaphore done_semas[n_threads];
  char names[n_threads][16];

  int i;
  for (i = 0; i < n_threads; i++)
  {
    snprintf (names[i], 16, "cache_thread_%i", i);
    sema_init (&done_semas[i], 0);
    thread_create (names[i], PRI_DEFAULT, cache_self_test_thread, &done_semas[i]);
  }

  /* Wait for completion. */
  for (i = 0; i < n_threads; i++)
    sema_down (&done_semas[i]);
}
#endif

/* TODO Once this is "working", remove the filesys_lock() and filesys_unlock()
   from the rest of the code base. */
