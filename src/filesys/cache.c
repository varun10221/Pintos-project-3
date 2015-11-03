#include "cache.h"

#include <string.h>

#ifdef CACHE_DEBUG
#include <stdio.h>
#include <random.h>
#include <stdlib.h>
#endif

#include <hash.h>

#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "devices/timer.h"
#include "devices/block.h"

#define CACHE_BLOCK_MAGIC 123456789

/* Which one of the buffer_cache's lists is this cache block in? */
enum cache_block_location
{
  CACHE_BLOCK_FREE_LIST,
  CACHE_BLOCK_IN_USE_LIST
};

/* Locking order: 
   1. Lock buffer_cache
   2. Lock cache_block */

/* Entry in a buffer_cache_table. In-memory cached version of on-disk data. */
struct cache_block
{
  /* For debugging. */
  int id;
  enum cache_block_location location;
  int magic;

  block_sector_t block;         /* Block address */
  enum cache_block_type type;   /* Inode, data, or metadata? */

  /* Copies of block and type, set in cache_get_block on a newly-obtained block. For eviction. */
  block_sector_t orig_block;
  enum cache_block_type orig_type;

  /* Acquire to modify this cache_block. Used as the "monitor lock" for the condition variables below. 
     Minimize time during which you hold this lock -- in particular do not hold it while doing disk IO,
     lest this cause a bottleneck in cache_get_block, where the algorithm is roughly:
          - new process calls cache_lock
          - new process finds cb in hash
          - new process calls cache_block_lock --> hang here waiting for disk IO, while holding cache_lock
          - new process calls cache_unlock
     To prevent this, make use of flags and condition variables. */
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

     To evict a block: Remove the block from the hash (so no new processes can use it), then wait until 
       there are no active users and no pending users. Then modify block as desired.

     There are inefficiencies in this approach, but it should prevent starvation and provide some fairness. */

  uint32_t readers_count;       /* No. of readers reading the block. */
  bool is_writer;               /* Is there a writer present? */
  /* Is block being prepared (by cache_evict_block)? If not, wait on cb_ready.
     NOTE: If you wait on cb_ready (in cache_get_block), then the block might have been evicted by the time you are signal'd.
     If so, start over. */
  bool is_being_prepared;       

  struct condition starving_process;  /* A process that would like to use the block, but cannot due to a conflicting use. */
  bool is_starving_process;           /* A starving process may not run immediately after being signal'd. This variable keeps others from beating him to the lock. */
  struct condition polite_processes;  /* If there is a waiter on the starving_process condition, other processes queue up on this condition variable to give the starving process a turn. */
  struct condition cb_ready; /* These processes found the block in the hash table, but it's not safe yet (because someone else cache_get_block'd too and hasn't finished setting it up). They wait until it is safe. */

  void *contents;               /* Pointer to the contents of this block. */
  bool has_cached_info;         /* Does this block contain cached information? (cb->contents can be NULL). This is equivalent to "is in buffer_cache's hash". */

  struct list_elem l_elem; /* For inclusion in one of the buffer_cache_table's lists. Protected by buffer_cache's lock. */
  struct hash_elem h_elem; /* For inclusion in the buffer_cache_table's lookup hash. Protected by buffer_cache's lock. */
};

/* A global buffer_cache_table stores cache_blocks. */
#define CACHE_SIZE 64
struct buffer_cache_table
{
  /* Invariant: when lock is released, all CACHE_SIZE blocks must be in one of these two lists. 
     Use cache_all_blocks_accounted_for to test this. */
  struct list free_blocks;        /* List of free cache blocks. These are blocks with no current users. They may have contents; if evicting then remove from hash. Never-used blocks are at the head and the block whose last user most recently put'd it is at the tail. These blocks are eligible for replacement. */
  struct list in_use_blocks;      /* List of in-use cache blocks with one or more users. Blocks might have 0 users and be in state is_being_prepared. */

  struct condition any_free_blocks; /* If no free blocks are found in cache_get_block, wait on this until ready. Could also PANIC. */

  struct cache_block blocks[CACHE_SIZE]; /* The cache_blocks we use. */
  struct hash addr_to_block;             /* Maps address to cache_block. */

  /* Serializes access to struct members. Hold for minimum amount of time to encourage parallelism. 
     In particular, DO NOT hold it while evicting a block or populating a block. */
  struct lock lock;

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

static bool cache_locked_by_me (void);
static void cache_lock (void);
static void cache_unlock (void);
static bool cache_all_blocks_accounted_for (void);
static void cache_evict_block (struct cache_block *);
static void cache_flush_block (struct cache_block *);
static void cache_mark_block_clean (struct cache_block *);
static void cache_discard_all (void);

/* Cache block. */
static void cache_block_init (struct cache_block *);
static void cache_block_get_access (struct cache_block *, bool);
static bool cache_block_is_usage_conflict (const struct cache_block *, bool);
static size_t cache_block_alloc_contents (struct cache_block *);
static void cache_block_destroy (struct cache_block *);
static void cache_block_set_hash (struct cache_block *, block_sector_t, enum cache_block_type);
static unsigned cache_block_hash_func (const struct hash_elem *, void *);
static bool cache_block_less_func (const struct hash_elem *, const struct hash_elem *, void *);
static size_t cache_block_get_contents_size (const struct cache_block *);
static size_t cache_block_get_n_sectors (const struct cache_block *);
static bool cache_block_in_use (const struct cache_block *);

static bool cache_block_locked_by_me (const struct cache_block *);
static void cache_block_lock (struct cache_block *);
static void cache_block_unlock (struct cache_block *);

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
     and Down'd by the readahead thread (to wait for a new item). 
   The cond and the bool are protected by the lock, and are used
     for synchronization with cache_discard. */
static struct list readahead_queue;
static struct lock readahead_queue_lock;
static struct semaphore readahead_queue_items_available;
static struct condition readahead_finished_item;
static bool readahead_handling_item;

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
  bdflush_args.flush_freq = 10; 
  bdflush_tid = thread_create ("bdflushd", PRI_DEFAULT, bdflush, &bdflush_args);
  ASSERT (bdflush_tid != TID_ERROR);

  /* Create the readahead thread. */
  /* Initialize the readahead fields used by the readahead thread and cache_discard. */
  list_init (&readahead_queue);
  lock_init (&readahead_queue_lock);
  sema_init (&readahead_queue_items_available, 0);
  cond_init (&readahead_finished_item);
  readahead_handling_item = false;

  struct semaphore readahead_started;
  sema_init (&readahead_started, 0);
  readahead_tid = thread_create ("readaheadd", PRI_DEFAULT, readahead, &readahead_started);
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
  /* TODO This is not a great implementation, but at least we flush all the dirty data to disk on our way out! */
  cache_flush ();
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

  cond_init (&buffer_cache.any_free_blocks);
  hash_init (&buffer_cache.addr_to_block, cache_block_hash_func, cache_block_less_func, NULL);

  /* Initialize each cache block. */
  int i;
  for (i = 0; i < CACHE_SIZE; i++)
  {
    struct cache_block *cb = &buffer_cache.blocks[i];
    cache_block_init (cb);
    cb->id = i;
    cb->location = CACHE_BLOCK_FREE_LIST;
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
   Caller must hold lock on buffer cache. 
   Returns the locked victim. 
   
   On return, victim is in buffer_cache's in_use_blocks list. */
static struct cache_block * 
buffer_cache_get_eviction_victim (void)
{
  /* TODO "You must implement a cache replacement algorithm that is at least as good as the "clock" algorithm. We encourage you to account for the generally greater value of metadata compared to data. Experiment to see what combination of accessed, dirty, and other information results in the best performance, as measured by the number of disk accesses." */

  ASSERT (cache_locked_by_me ());

  struct cache_block *cb;

  /* We evict from free_blocks. */
  ASSERT (!list_empty (&buffer_cache.free_blocks));
  /* Any "truly free" blocks (those that do not represent a once-used on-disk block) are at the head. */
  cb = list_entry (list_pop_front (&buffer_cache.free_blocks), struct cache_block, l_elem);
  ASSERT (cb->location == CACHE_BLOCK_FREE_LIST);

  /* This block is now in use. */
  cb->location = CACHE_BLOCK_IN_USE_LIST;
  list_push_front (&buffer_cache.in_use_blocks, &cb->l_elem);

  cache_block_lock (cb);
  return cb;
}

/* Initialize this cache block. */
static void 
cache_block_init (struct cache_block *cb)
{
  ASSERT (cb != NULL);
  memset (cb, 0, sizeof(struct cache_block));

  cb->magic = CACHE_BLOCK_MAGIC;

  lock_init (&cb->lock);
  cond_init (&cb->starving_process);
  cb->is_starving_process = false;
  cond_init (&cb->polite_processes);
  cond_init (&cb->cb_ready);

  cb->is_being_prepared = false;
  cb->contents = NULL;
  cb->has_cached_info = false;
  /* 0 readers and no writer: Not in use. */
  cb->readers_count = 0;
  cb->is_writer = false;
}

/* Obtain the appropriate access to locked CB.
   Waits until CB is available.
   Caller must hold the lock on CB. 
   Caller should NOT hold the lock on the cache. */
static void 
cache_block_get_access (struct cache_block *cb, bool exclusive)
{
  ASSERT (cb != NULL);
  ASSERT (cache_block_locked_by_me (cb));
  ASSERT (!cache_locked_by_me ());

  ASSERT (!cb->is_being_prepared);
  ASSERT (cb->location == CACHE_BLOCK_IN_USE_LIST);

  while (cb->is_starving_process)
    /* While there is a starving process, wait politely. */
    cond_wait (&cb->polite_processes, &cb->lock);
  ASSERT (cond_n_waiters (&cb->starving_process, &cb->lock) == 0);

  /* No starving process. Graduate to being the starving process if conflict. */
  if (cache_block_is_usage_conflict (cb, exclusive))
  {
    /* If there is a conflict, wait as the starving process. 
       1. Only one process can become the starving process at a time,
       2. We only cond_signal() the starving_process condition when the usage can change.
       3. Until the starving process wakes up, everyone else waits on polite_processes.
       Consequently, when I am woken there should no longer be a conflict. */
    cb->is_starving_process = true;
    cond_wait (&cb->starving_process, &cb->lock);
    cb->is_starving_process = false;
  }

  /* There must now be no conflict between my intended usage and the block status. */
  ASSERT (!cache_block_is_usage_conflict (cb, exclusive));
  if (exclusive)
    cb->is_writer = true;
  else
    cb->readers_count++;

  ASSERT (cache_block_in_use (cb));
}

/* Returns true if there is a conflict between EXCLUSIVE and current block usage.
   CB must be locked. */
static bool 
cache_block_is_usage_conflict (const struct cache_block *cb, bool exclusive)
{
  ASSERT (cb != NULL);
  ASSERT (cache_block_locked_by_me (cb));

  bool is_conflict = (exclusive && cache_block_in_use (cb)) || 
                     (!exclusive && cb->is_writer);
  return is_conflict;
}

/* Destroy this cache block. */
static void 
cache_block_destroy (struct cache_block *cb)
{
  ASSERT (cb != NULL);
}

/* Dynamically allocate the contents field of locked CB. 
   Requires cb->type to be set appropriately. 
   Returns the size of contents. */
static size_t 
cache_block_alloc_contents (struct cache_block *cb)
{
  ASSERT (cb != NULL);
  ASSERT (cb->contents == NULL);
  ASSERT (cache_block_locked_by_me (cb));

  size_t n_bytes = cache_block_get_contents_size (cb);
  cb->contents = malloc (n_bytes);
  ASSERT (cb->contents != NULL);

  return n_bytes;
}

/* Evict the contents of unlocked CB so that another block can be stored in it. 
   The caller should already have updated CB->BLOCK and CB->TYPE and placed CB in the hash 
   under its "new name", though it should NOT have updated CB->ORIG_BLOCK and CB->ORIG_TYPE 
   because we use those to flush contents if CB is dirty. 
   
   CB should be marked as is_being_prepared. */
static void 
cache_evict_block (struct cache_block *cb)
{
  ASSERT (cb != NULL);
  ASSERT (!cache_block_locked_by_me (cb));
  ASSERT (cb->is_being_prepared);

  /* Evict. */
  if (cb->contents != NULL)
  {
    if (cb->is_dirty)
      cache_flush_block (cb);
    free (cb->contents);
    cb->contents = NULL;
  }
}

/* Flush this dirty CB to disk.
   CB does not need to be locked (may be being prepared), but caller
   should ensure that access to this method is synchronized. */
static void 
cache_flush_block (struct cache_block *cb)
{
  ASSERT (cb != NULL);
  ASSERT (cb->is_dirty);
  ASSERT (cb->contents != NULL);

  size_t n_sectors = cache_block_get_n_sectors (cb);
  size_t i;
  for (i = 0; i < n_sectors; i++)
  {
    size_t rel_offset = i*BLOCK_SECTOR_SIZE;
    block_write (buffer_cache.backing_block, cb->orig_block + rel_offset, cb->contents + rel_offset);
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
  /* Pointer to the CB matching this address and type. */
  struct cache_block *cb = NULL;

  /* Prepare to search hash. */
  struct cache_block dummy;
  cache_block_set_hash (&dummy, address, type);

  ASSERT (!cache_locked_by_me ());
  bool need_to_lock_cache = true;

/* TODO Debugging code. */
#ifdef CACHE_DEBUG
  int tid = thread_current ()->tid;
#endif

  /* Loop getting the CB we need, while competing with eviction.
     At the end of the loop, we have a locked CB and an unlocked cache. */ 
  while (cb == NULL)
  {
    /* If we're coming from the cond_wait below, we already have the lock. */
    if (need_to_lock_cache)
      cache_lock ();
    else
      /* Default stance is to lock the cache. */
      need_to_lock_cache = true;
    ASSERT (cache_locked_by_me ());

    #ifdef CACHE_DEBUG
      printf ("thread %i: Cache is locked\n", tid);
    #endif

    /* OK, now we have the cache locked.
       Options: - block is cached already
                - no block, but there's a free block
                - no block, but there's an in-use block we can evict
                - no block, no free blocks, no in-use block: wait and try again */

    /* Is this block already in the cache? */
    struct hash_elem *match = hash_find (&buffer_cache.addr_to_block, &dummy.h_elem);
    if (match)
    {
      /* Already in the hash. Lock it, update LRU status, and wait until it is valid. */
      cb = hash_entry (match, struct cache_block, h_elem);
    #ifdef CACHE_DEBUG
      printf ("thread %i: found matching block in cache: %i.\n", tid, cb->id);
    #endif
      cache_block_lock (cb);

      /* If this CB was on the free list, move it to the in_use list. */
      if (cb->location == CACHE_BLOCK_FREE_LIST)
      {
        list_remove (&cb->l_elem);
        cb->location = CACHE_BLOCK_IN_USE_LIST;
        list_push_front (&buffer_cache.in_use_blocks, &cb->l_elem);
      }

      /* We have CB locked, so we can safely unlock the cache. */
      cache_unlock ();

      /* Wait until CB is ready to use. Another process might be evicting the old block
         from this CB. */
      while (cb->is_being_prepared)
        cond_wait (&cb->cb_ready, &cb->lock);

      /* CB is ready. Is it still what we thought it was? Might have been itself evicted, "changing shape" 
           between it becoming ready and our acquisition of the lock. */
      if (cb->block != address || cb->type != type)
      {
        /* Bummer. Retry. */
      #ifdef CACHE_DEBUG
        printf ("thread %i: CB has changed. Retrying. \n", tid);
      #endif
        cache_block_unlock (cb);
        cb = NULL;
        need_to_lock_cache = true;
        continue;
      }
      /* We hold CB lock, so it cannot be evicted. */
      /* Ready to exit the loop. */
    }
    else if (!list_empty (&buffer_cache.free_blocks))
    {
      cb = buffer_cache_get_eviction_victim ();
      ASSERT (cb->location == CACHE_BLOCK_IN_USE_LIST);
      ASSERT (cache_block_locked_by_me (cb));
      ASSERT (!cb->is_being_prepared);
      /* CB is now in in_use_blocks and thus safe from being evicted. */ 

    #ifdef CACHE_DEBUG
      printf ("thread %i: found a victim block: %i. Locking it.\n", tid, cb->id);
    #endif

      /* Remove "old" block from the cache's addr table so no new users
           will be able to find it. */
      if (cb->has_cached_info)
        hash_delete (&buffer_cache.addr_to_block, &cb->h_elem);   

      cb->is_being_prepared = true;

      /* Insert the new block into the hash so others can find it. */
      cb->block = address;
      cb->type = type;
      hash_insert (&buffer_cache.addr_to_block, &cb->h_elem);
      cb->has_cached_info = true;

      /* We have CB locked, it's marked is_being_prepared, and it's in in_use_blocks.
         It is therefore safe from eviction, and any new users will wait. 
         We can drop the locks now, wait for current users to finish, and do IO. */
      cache_unlock ();
      cache_block_unlock (cb);

      /* Evict old contents of CB. May require disk IO. */
      cache_evict_block (cb);

      /* Now CB has been evicted. Re-acquire lock, mark ready, and signal waiters. */
      cache_block_lock (cb);

      /* No one else should have been here in our absence. */
      ASSERT (cb->location == CACHE_BLOCK_IN_USE_LIST);
      ASSERT (!cache_block_in_use (cb));
      ASSERT (cb->is_being_prepared);
      /* There should be no one waiting on these conditions, because it has been is_being_prepared. */
      ASSERT (cond_n_waiters (&cb->starving_process, &cb->lock) == 0);
      ASSERT (cond_n_waiters (&cb->polite_processes, &cb->lock) == 0);

      /* CB is now prepared: if its contents were being evicted, they are now gone. 
           It is not valid, though. */
      cb->is_being_prepared = false;
      ASSERT (cb->contents == NULL);

      /* Update these for later evicters. */
      cb->orig_block = address;
      cb->orig_type = type;

      /* Signal anyone waiting on cb_ready. */
      cond_broadcast (&cb->cb_ready, &cb->lock);

      /* We hold CB lock (so it cannot be evicted). */
      /* Ready to exit the loop. */
    }
    else
    {
    #ifdef CACHE_DEBUG
      printf ("thread %i: found no blocks. Trying again.\n", tid);
    #endif
      /* No match and no free blocks to evict.
         Wait until there is a free block. 

         During functional testing, this is indicative of a missing cache_put_block. 
         During "real" use, it could happen if there are more than CACHE_SIZE concurrent users. */
      /* TODO Revisit this. */
#if 0
      PANIC ("cache_get_block: no free blocks. This suggests a missing cache_put_block.\n");
#else
      cond_wait (&buffer_cache.any_free_blocks, &buffer_cache.lock);
      /* Try again, holding onto the buffer_cache.lock -- dropping it would be counterproductive.
         We've already released and re-acquired it once. */
      cb = NULL;
      need_to_lock_cache = false;
#endif
      continue;
    }
  } /* Loop waiting for CB. */

  /* Ensure that we don't have cache locked and that we do have CB locked. */
  ASSERT (!cache_locked_by_me ());
  ASSERT (cb != NULL);
  ASSERT (cache_block_locked_by_me (cb));

  /* Update CB to indicate that we are using it. */
  cache_block_get_access (cb, exclusive);
  cache_block_unlock (cb);

#ifdef CACHE_DEBUG
  printf ("cache_get_block: thread %i returning cb %i\n", tid, cb->id);
#endif

  return cb;
}

/* Release access to this cache block. 
   starving_process and polite_processes conditions are signaled appropriately.

   If we were the last user, CB is placed at the tail of the buffer_cache's free_list.
   Acquires and releases lock on buffer cache and CB. */
void 
cache_put_block (struct cache_block *cb)
{
  ASSERT (cb != NULL);

  cache_lock ();
  cache_block_lock (cb);

  ASSERT (cache_block_in_use (cb));
  ASSERT (cb->location == CACHE_BLOCK_IN_USE_LIST);

  bool any_other_users = true;
  /* If there's a writer, that's me and I'm done. */
  if (cb->is_writer)
  {
    cb->is_writer = false;
    any_other_users = false;
  }
  /* Else I am one of the readers. */
  else if (0 < cb->readers_count)
  {
    cb->readers_count--;
    if (cb->readers_count == 0)
      any_other_users = false;
  }
  else
    NOT_REACHED ();

  /* If CB is now empty of users, signal starving_process if there is one,
     otherwise broadcast to polite_processes if there are any,
     otherwise move onto free_list. */
  if (!any_other_users)
  {
    if (cb->is_starving_process)
      cond_signal (&cb->starving_process, &cb->lock);
    else if (cond_n_waiters (&cb->polite_processes, &cb->lock))
      cond_broadcast (&cb->polite_processes, &cb->lock);
    else
    {
      /* No current or pending processes, so put at the *back* of the free list.
         Leave in the hash, though, so that new users can still find it. */
      ASSERT (cb->location == CACHE_BLOCK_IN_USE_LIST);
      list_remove (&cb->l_elem);
      cb->location = CACHE_BLOCK_FREE_LIST;
      list_push_back (&buffer_cache.free_blocks, &cb->l_elem);
      /* There's now a free block! */
      cond_signal (&buffer_cache.any_free_blocks, &buffer_cache.lock);
    }
  }

  cache_block_unlock (cb);
  cache_unlock ();
}

/* (If CB is not valid, fill CB with data and) return pointer to the data. 
   CB->TYPE and CB->BLOCK must be set correctly. */
void * 
cache_read_block (struct cache_block *cb)
{
  ASSERT (cb != NULL);
  size_t n_sectors, rel_offset, i;

#ifdef CACHE_DEBUG
  /* Check for lock leak. */
  ASSERT (!cache_block_locked_by_me (cb));
#endif
  cache_block_lock (cb);

  ASSERT (cache_block_in_use (cb));

  if (cb->contents == NULL)
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
  }

  cache_block_unlock (cb);
  return cb->contents;
}

/* Fill CB with zeros and return pointer to the data. 
   CB->TYPE must be set correctly.
   CB is marked dirty. There must be a writer of CB. */
void * 
cache_zero_block (struct cache_block *cb)
{
  ASSERT (cb != NULL);

#ifdef CACHE_DEBUG
  /* Check for lock leak. */
  ASSERT (!cache_block_locked_by_me (cb));
#endif
  cache_block_lock (cb);

  ASSERT (cb->is_writer);

  if (cb->contents == NULL)
  {
    /* Allocate contents. */
    cache_block_alloc_contents (cb);
    ASSERT (cb->contents != NULL);
  }
  size_t n_bytes = cache_block_get_contents_size (cb);
  memset (cb->contents, 0, n_bytes);
  cb->is_dirty = true;

  cache_block_unlock (cb);
  return cb->contents;
}

/* Flush all dirty blocks from the cache. */
void 
cache_flush (void)
{
  int i;
  for (i = 0; i < CACHE_SIZE; i++)
  {
    struct cache_block *cb = &buffer_cache.blocks[i];
    /* Only flush dirty blocks. Optimistic search. */
    if (cb->contents != NULL && cb->is_dirty)
    {
      cache_block_lock (cb);
      if (cb->is_dirty) 
        cache_flush_block (cb);
      cache_block_unlock (cb);
    }
  }
}

/* Mark CB dirty. There must be a writer. */
void 
cache_mark_block_dirty (struct cache_block *cb)
{
  ASSERT (cb != NULL);

  cache_block_lock (cb);

  ASSERT (cb->is_writer);
  ASSERT (cb->contents != NULL);

  cb->is_dirty = true;

  cache_block_unlock (cb);
}

/* Internal function. Mark CB clean. Use with cache_flush_block. 
   CB does not need to be locked (may be being prepared), but caller
   should ensure that access to this is synchronized. */
static void
cache_mark_block_clean (struct cache_block *cb)
{
  ASSERT (cb != NULL);

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
   Hash is the hash of the CB's block and type fields. 
   Keep in sync with cache_block_set_hash. */ 
static unsigned 
cache_block_hash_func (const struct hash_elem *cb_elem, void *aux UNUSED)
{
  ASSERT (cb_elem != NULL);
  struct cache_block *cb = hash_entry (cb_elem, struct cache_block, h_elem);
  ASSERT (cb != NULL);

  return hash_bytes (&cb->block, sizeof(block_sector_t)) ^ hash_int (cb->type);
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

/* Return true if CB is in use, else false. 
   Caller is advised to keep CB locked or otherwise
   synchronize access. */
static bool 
cache_block_in_use (const struct cache_block *cb)
{
  ASSERT (cb != NULL);

  bool writer = cb->is_writer;
  bool readers = (0 < cb->readers_count);
  /* Can't be both! */
  ASSERT (! (writer && readers) );
  return (writer || readers);
}

/* Return true if I have this CB locked, else false. */
static bool 
cache_block_locked_by_me (const struct cache_block *cb)
{
  ASSERT (cb != NULL);

  return lock_held_by_current_thread (&cb->lock);
}

/* Lock this cache_block. Also tests invariants. */
static void 
cache_block_lock (struct cache_block *cb)
{
  ASSERT (cb != NULL);
  ASSERT (!lock_held_by_current_thread (&cb->lock));

#ifdef CACHE_DEBUG
  tid_t tid = thread_current ()->tid;
  printf ("cache_block_lock: thread %i locking cb %i\n", tid, cb->id);
#endif

  lock_acquire (&cb->lock);
  ASSERT (cb->magic == CACHE_BLOCK_MAGIC);

#ifdef CACHE_DEBUG
  printf ("cache_block_lock: thread %i locked cb %i\n", tid, cb->id);
#endif
}

/* Unlock this cache_block. If CACHE_DEBUG is enabled, may also test invariants. */
static void 
cache_block_unlock (struct cache_block *cb)
{
  ASSERT (cb != NULL);
  ASSERT (lock_held_by_current_thread (&cb->lock));

  ASSERT (cb->magic == CACHE_BLOCK_MAGIC);
  lock_release (&cb->lock);

#ifdef CACHE_DEBUG
  tid_t tid = thread_current ()->tid;
  printf ("cache_block_lock: thread %i unlocked cb %i\n", tid, cb->id);
#endif
}

/* Return the size in bytes of CB->CONTENTS. */
static size_t
cache_block_get_contents_size (const struct cache_block *cb)
{
  ASSERT (cb != NULL);

  size_t n_bytes = 0;
  switch (cb->orig_type)
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

   This request may come from the calling process before the 
   readahead thread has a chance to do it. */
void 
cache_readahead (block_sector_t address, enum cache_block_type type)
{
  /* This mem is freed by the readahead thread. */
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

/* Must be called before marking this block free.
   If a cache block with this {address, type} pair is cached,
     eliminate it.

   Motivation: consider the following scenario:

    File A is deleted, and its blocks are marked free. Its first block is still
      cached.
    File B is created, and re-uses A's cached block. A user calls read(),
      we find the block in the cache, and we return A's old data to the reader
      of B. 
    To avoid this scenario:
      When deleting A, before we mark its blocks free, we call cache_discard.
      Now any re-use of A's blocks will result in a cache miss. 

   This should only be used when protected by filesystem-level synchronization.
   HOWEVER there may have been an old readahead request pending. 
   We remove any we can find, but if readahead has begun to process one already, 
     we have to dance around it.

   Locking scheme: As the flush is expensive, we avoid holding the lock on the cache unless we need it.
     To minimize lock time on the buffer_cache, we lock the buffer cache, 
       find and lock the match, do bookkeeping, and then unlock the cache.
     Once the cache is unlocked, no other process can find the match in the hash.
     The contract promises that no other process will ATTEMPT to find the match in the hash,
       so no worries about holding onto the CB lock during flush.
     We then flush the match, unlock the match, re-lock the cache, and update the list. */
void 
cache_discard (block_sector_t address, enum cache_block_type type)
{
  /* First, remove any pending readahead requests for this item from the readahead_queue. 
     Due to the contract of this function, any pending readahead requests are stale and the user
     no longer needs them fulfilled. */
  /* TODO Make this a function. */
  lock_acquire (&readahead_queue_lock);

  struct list_elem *e = list_begin (&readahead_queue);
  while (e != list_end (&readahead_queue))
  {
    readahead_request_t *req = list_entry (e, readahead_request_t, elem);
    struct list_elem *e_next = list_next (e);
    if (req->address == address && req->type == type)
    {
      list_remove (e);
      free (req);
      sema_down (&readahead_queue_items_available);
    }
    e = e_next;
  }

  /* If readahead is currently being performed, wait until it finishes. 
     We don't want to race with a readahead on this block!

     Since we deleted all other corresponding readahead requests, 
       we only wait once. */
  if (readahead_handling_item)
    cond_wait (&readahead_finished_item, &readahead_queue_lock);
  lock_release (&readahead_queue_lock);

  /* Prepare to search hash. */
  struct cache_block *cb = NULL;
  struct cache_block dummy;
  cache_block_set_hash (&dummy, address, type);

  cache_lock ();

  /* Check for a match. */
  struct hash_elem *match = hash_find (&buffer_cache.addr_to_block, &dummy.h_elem);
  if (match)
  {
    /* Match found. We have work to do. */
    cb = hash_entry (match, struct cache_block, h_elem);
    /* Don't bother locking CB: per contract no one else is using it or wants it. */
    /* TODO Perhaps mark CB as being_discarded so that we can assert about it in other places? */

    ASSERT (cb->has_cached_info);
    ASSERT (!cache_block_in_use (cb));

    /* Remove from hash. Now no one in cache_get_block will find it. */
    hash_delete (&buffer_cache.addr_to_block, &cb->h_elem);   
    cb->has_cached_info = false;

    /* Since we want to unlock the cache, move the CB to the IN_USE list until we flush it. */
    if (cb->location != CACHE_BLOCK_IN_USE_LIST)
    {
      /* TODO A CB 'move' function that does this for us? */
      list_remove (&cb->l_elem);
      cb->location = CACHE_BLOCK_IN_USE_LIST;
      list_push_back (&buffer_cache.in_use_blocks, &cb->l_elem);
    }

    cache_unlock ();

    /* Flush. */
    if (cb->contents != NULL)
    {
      if (cb->is_dirty)
        cache_flush_block (cb);
      free (cb->contents);
      cb->contents = NULL;
    }

    /* Check for race conditions -- violation of contract. */
    ASSERT (!cache_block_in_use (cb));
    /* Now this CB is "truly free", so put at the *front* of the free list. */
    cache_lock ();
    ASSERT (cb->location == CACHE_BLOCK_IN_USE_LIST);
    list_remove (&cb->l_elem);
    cb->location = CACHE_BLOCK_FREE_LIST;
    list_push_front (&buffer_cache.free_blocks, &cb->l_elem);
    cond_signal (&buffer_cache.any_free_blocks, &buffer_cache.lock);
    cache_unlock ();
  }
  else
    /* No match, nothing to do. */
    cache_unlock ();
}

/* Discard all blocks from the cache.
   This is a test function in support of thread_cache_self_test. */
static void 
cache_discard_all (void)
{
  int i;
  for (i = 0; i < CACHE_SIZE; i++)
  {
    struct cache_block *cb = &buffer_cache.blocks[i];
    cache_discard (cb->block, cb->type);
  }
}

/* Return true if I hold the lock on the buffer cache, else false. */
static bool 
cache_locked_by_me (void)
{
  return lock_held_by_current_thread (&buffer_cache.lock);
}

/* Lock the buffer cache. If CACHE_DEBUG is enabled, also tests invariants. */
static void 
cache_lock (void)
{
  ASSERT (!lock_held_by_current_thread (&buffer_cache.lock));

#ifdef CACHE_DEBUG
  tid_t tid = thread_current ()->tid;
  printf ("cache_lock: thread %i locking cache\n", tid);
#endif

  lock_acquire (&buffer_cache.lock);
#ifdef CACHE_DEBUG
  ASSERT (cache_all_blocks_accounted_for ());
#endif

#ifdef CACHE_DEBUG
  printf ("cache_lock: thread %i locked cache\n", tid);
#endif
}

/* Unlock the buffer cache. If CACHE_DEBUG is enabled, also tests invariants. */
static void 
cache_unlock (void)
{
  ASSERT (lock_held_by_current_thread (&buffer_cache.lock));

#ifdef CACHE_DEBUG
  ASSERT (cache_all_blocks_accounted_for ());
#endif
  lock_release (&buffer_cache.lock);

#ifdef CACHE_DEBUG
  tid_t tid = thread_current ()->tid;
  printf ("cache_lock: thread %i unlocked cache\n", tid);
#endif
}

/* Return true if all CACHE_SIZE blocks are in one of the 3 lists, else false.
   Caller must hold lock on buffer cache. */
static bool 
cache_all_blocks_accounted_for (void)
{
  size_t n_blocks = list_size (&buffer_cache.free_blocks) + list_size (&buffer_cache.in_use_blocks); 
  return (n_blocks == CACHE_SIZE);
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
    readahead_handling_item = true;
    ASSERT (!list_empty (&readahead_queue));
    readahead_request_t *req = list_entry (list_pop_front (&readahead_queue), readahead_request_t, elem);
    lock_release (&readahead_queue_lock);
    ASSERT (req != NULL);

    /* Fulfill the request. */
    struct cache_block *cb = cache_get_block (req->address, req->type, false);
    ASSERT (cb != NULL);

    /* If the block hasn't been read yet, read it now. */
    if (cb->contents == NULL)
      cache_read_block (cb);
    /* We are not using the block right now, so it can be evicted if need be.
        Hopefully it is not evicted before it is cache_get_block'd by whoever 
        asked for it. */
    cache_put_block (cb);

    free (req);

    /* Tell anyone who was listening in cache_discard that we are done handling
       this request. */
    lock_acquire (&readahead_queue_lock);
    readahead_handling_item = false;
    cond_broadcast (&readahead_finished_item, &readahead_queue_lock);
    lock_release (&readahead_queue_lock);
  }

}

#ifdef CACHE_DEBUG
/* This is a test for the block cache. 
   Cache must have been cache_init()'d before calling this. 
   Hammers on the cache. 
   Uses a single size throughout to avoid type conflict:
     The addr_to_block hash uses the block address as the hash value.
     If the type of a block changes, cache_discard must have been
     called in a controlled manner. Since this is a "hammer" test,
     we have no way to controlled-ly call cache_discard.

   Client for cache_self_test_{single, parallel}. */
static void 
cache_self_test_thread (void *args)
{
  struct semaphore *is_done = (struct semaphore *) args;

  int i;
  struct cache_block *cb = NULL;
  int tid = thread_current ()->tid;

  /* Get CACHE_SIZE unique exclusive blocks. */
  for (i = 0; i < CACHE_SIZE; i++)
  {
    printf ("cache_self_test_thread: thread %i: iter %i: Getting block (%i, CACHE_BLOCK_INODE, true)\n", tid, i, i);
    cb = cache_get_block (i, CACHE_BLOCK_INODE, true);
    cache_put_block (cb);
  }

  /* Get the same (new!) block 3*CACHE_SIZE times, shared. 
     The first get* should cause eviction. */
  for (i = 0; i < 3*CACHE_SIZE; i++)
  {
    printf ("cache_self_test_thread: thread %i: iter %i: Getting block (%i, CACHE_BLOCK_INODE, false)\n", tid, i, CACHE_SIZE);
    cb = cache_get_block (CACHE_SIZE, CACHE_BLOCK_INODE, false);
    cache_put_block (cb);
  }

  cb = cache_get_block (i, CACHE_BLOCK_INODE, true);
  char *data = cache_read_block (cb);
  cache_put_block (cb);

  /* Get a random block with random exclusive value. */
  int n_rand_iters;
  //n_rand_iters = MAX(10*CACHE_SIZE, 500); /* TODO */
  //n_rand_iters = 200;
  n_rand_iters = 50;
  for (i = 0; i < n_rand_iters; i++)
  {
    int block = random_ulong () % (2*CACHE_SIZE);
    bool exclusive = (random_ulong () % 2) ? true : false;
    printf ("cache_self_test_thread: thread %i: random iter %i: Getting block (%i, CACHE_BLOCK_INODE, %i)\n", tid, i, block, exclusive);
    cb = cache_get_block (block, CACHE_BLOCK_INODE, exclusive); 
    char *data = cache_read_block (cb);
    cache_put_block (cb);
  }

  /* Test readahead. */
  for (i = 0; i < 2*CACHE_SIZE; i++)
  {
    printf ("cache_self_test_thread: thread %i: readahead iter %i: requesting block (%i, CACHE_BLOCK_INODE)\n", tid, i, i);
    cache_readahead (i, CACHE_BLOCK_INODE);
  }

  /* Test discard -- must be a block nobody has used yet, since there's no
       higher-level synchronization here. */
  int discard_begin = 999999999;
  for (i = 0; i < 2*CACHE_SIZE; i++)
  {
    printf ("cache_self_test_thread: thread %i: discard iter %i: requesting block (%i, CACHE_BLOCK_INODE)\n", tid, i, discard_begin + i);
    cache_discard (discard_begin + i, CACHE_BLOCK_INODE);
  }

  /* Flush the cache (this should be a no-op, as we haven't modified anything). */
  cache_flush ();
  sema_up (is_done);
}

/* Single-threaded test of the cache. */
static void 
cache_self_test_single (void)
{
  struct semaphore is_done;
  sema_init (&is_done, 0);

  printf ("cache_self_test_single: Launching one thread\n");
  cache_self_test_thread (&is_done);
  sema_down (&is_done);

  printf ("cache_self_test_single: Thread is done\n");

  /* Discard all to avoid discomfiting anything after us. */
  /* This will compete with readahead thread, which is fun. */
  cache_discard_all ();
  /* But make sure the cache is really empty before we return. */
  while (!list_empty (&readahead_queue))
    thread_yield ();
  cache_discard_all ();
  printf ("cache_self_test_single: Returning. Cache is empty.\n");
}

/* Multi-threaded test of the cache. */
static void
cache_self_test_parallel (void)
{
  int n_threads = 30;

  struct semaphore done_semas[n_threads];
  char names[n_threads][16];
  tid_t tids[n_threads];

  printf ("cache_self_test_parallel: Launching %i threads\n", n_threads);
  int i;
  for (i = 0; i < n_threads; i++)
  {
    snprintf (names[i], 16, "cache_thread_%i", i);
    sema_init (&done_semas[i], 0);
    tids[i] = thread_create (names[i], PRI_DEFAULT, cache_self_test_thread, &done_semas[i]);
    printf ("cache_self_test_parallel: started thread %i\n", tids[i]);
  }

  /* Wait for completion. */
  for (i = 0; i < n_threads; i++)
  {
    sema_down (&done_semas[i]);
    printf ("cache_self_test_parallel: thread %i is done\n", tids[i]);
  }

  printf ("cache_self_test_parallel: All %i threads are done\n", n_threads);

  /* Discard all to avoid discomfiting anything after us. */
  /* This will compete with readahead thread, which is fun. */
  printf ("cache_self_test_parallel: Discarding all (compete with readahead)\n");
  cache_discard_all ();
  /* But make sure the cache is really empty before we return. */
  while (!list_empty (&readahead_queue))
    thread_yield ();
  printf ("cache_self_test_parallel: Discarding all (cleanup)\n");
  cache_discard_all ();
  printf ("cache_self_test_parallel: Returning. Cache is empty.\n");
}
#endif

/* TODO Once this is "working", remove the filesys_lock() and filesys_unlock()
   from the rest of the code base. */
