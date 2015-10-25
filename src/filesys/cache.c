#include "cache.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "devices/timer.h"

/* TODO remove this */
#include <stdio.h>

/* Private functions. */

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
