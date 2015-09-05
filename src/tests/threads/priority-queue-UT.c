/* Unit test for the priority queue data structure. */ 

#include <stdio.h>
#include <string.h>
#include "tests/threads/tests.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "devices/timer.h"

#define THR_PER_PRI 100
#define N_THREADS PRI_QUEUE_NLISTS*THR_PER_PRI

/* Borrowed from thread.c for our fake threads. */
#define THREAD_MAGIC 0xcd6abf4b

void
test_priority_queue_UT (void)
{
  struct priority_queue pq;
  priority_queue_init(&pq);

  if(pq.size != (size_t) 0)
    fail("Error, pq size is %i != 0\n", pq.size);

  struct thread *threads = (struct thread*) malloc(N_THREADS * sizeof(struct thread));
  if(threads == NULL)
    fail("Error, could not allocate memory\n");

  int i, j;
  int thr_ix = 0;

  /* Populate the priority queue. */
  for(i = 0; i < PRI_QUEUE_NLISTS; i++)
  {
    for(j = 0; j < THR_PER_PRI; j++)
    {
      struct thread *thr = &threads[thr_ix];
      memset(thr, 0, sizeof(struct thread));
      thr->magic = THREAD_MAGIC;
      thr->tid = thr_ix;
      thr->status = THREAD_RUNNING;
      snprintf(thr->name, 15, "thread %i", thr_ix);
      /* Start at highest priority and work our way down.
         This way thread 0 goes first, thread 1 second, and so on. */
      thr->base_priority = PRI_MAX - i; 
      thr->effective_priority = thr->base_priority;
      list_init (&thr->resource_list);

      priority_queue_push_back(&pq, thr);

      thr_ix++;
      if(pq.size != (size_t) thr_ix)
        fail("Error, pq size is %i != %i\n", pq.size, thr_ix);

    }
  }

  if(N_THREADS != thr_ix)
    fail("Internal error, N_THREADS %i != thr_ix %i\n", N_THREADS, thr_ix);

  size_t s = priority_queue_size(&pq);
  if(s != N_THREADS)
    fail("Error, priority queue size reported as %i != %i\n", s, N_THREADS);

  if(priority_queue_empty(&pq))
    fail("Error, priority queue is reported as empty despite containing %i threads\n", N_THREADS);

  /* Empty the priority queue. */
  for(i = 0; i < N_THREADS; i++)
  {
    struct thread *t = NULL;

    t = priority_queue_max(&pq);
    if(t->tid != i)
      fail("Error, max thread id is %i != %i\n", t->tid, i);

    t = priority_queue_pop_front(&pq);
    if(t->tid != i)
      fail("Error, popped thread id is %i != %i\n", t->tid, i);

    size_t s = priority_queue_size(&pq);
    size_t expected_size = N_THREADS - i - 1;
    if(s != expected_size)
      fail("Error, priority queue size reported as %i != %i\n", s, expected_size);
  }

  if(!priority_queue_empty(&pq))
    fail("Error, priority queue is reported as non-empty despite having just been emptied\n", thr_ix);

  pass ();
}
