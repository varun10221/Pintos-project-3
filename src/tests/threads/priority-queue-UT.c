/* Unit test for the priority queue data structure. */ 

#include <stdio.h>
#include <string.h>
#include "tests/threads/tests.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "devices/timer.h"

#define THR_PER_PRI 10
#define N_THREADS PRI_QUEUE_NLISTS*THR_PER_PRI

/* Borrowed from thread.c for our fake threads. */
#define THREAD_MAGIC 0xcd6abf4b

void
test_priority_queue_UT (void)
{
  struct priority_queue pq;
  priority_queue_init(&pq);
  size_t s;
  int64_t expected_tid;
  int64_t expected_size;
  struct thread *thr = NULL;

  if(pq.size != (size_t) 0)
    fail("Error, pq size is %i != 0\n", pq.size);

  struct thread *threads = (struct thread*) malloc(N_THREADS * sizeof(struct thread));
  if(threads == NULL)
    fail("Error, could not allocate memory\n");

  int i, j;
  int thr_ix = 0;

  /* Initialize the threads. */
  for(i = 0; i < PRI_QUEUE_NLISTS; i++)
  {
    for(j = 0; j < THR_PER_PRI; j++)
    {
      thr = &threads[thr_ix];
      memset(thr, 0, sizeof(struct thread));
      thr->magic = THREAD_MAGIC;
      thr->tid = thr_ix;
      thr->status = THREAD_RUNNING;
      snprintf(thr->name, 15, "thread %i", thr_ix);
      /* Start at highest priority and work our way down.
         This way thread 0 should be popped first, thread 1 second, and so on. */
      thr->base_priority = PRI_MAX - i; 
      thr->effective_priority = thr->base_priority;
      list_init (&thr->resource_list);
      thr->elem.p = &thr->effective_priority;

      /* Set wake_me_at values in inverse insertion order within a queue. */
      thr->wake_me_at = THR_PER_PRI - j - 1;
      thr->elem.sort_val = &thr->wake_me_at;

      thr_ix++;
    }
  }

  /* Populate the priority queue, unsorted. */
#ifdef DEBUG
  printf("Populating the priority queue, unsorted\n");
#endif
  for(i = 0; i < N_THREADS; i++)
  {
    thr = &threads[i];

#ifdef DEBUG
    printf("Inserting thread <%s>: priority %i wake_me_at %lli\n",
      thr->name, *thr->elem.p, *thr->elem.sort_val);
#endif
    priority_queue_push_back(&pq, &thr->elem);
    expected_size = i + 1;
    if(pq.size != expected_size)
      fail("Error, pq size is %i != %i\n", pq.size, expected_size);

    priority_queue_verify (&pq, false, NULL, NULL);
  }

  if(N_THREADS != thr_ix)
    fail("Internal error, N_THREADS %i != thr_ix %i\n", N_THREADS, thr_ix);

  s = priority_queue_size(&pq);
  if(s != N_THREADS)
    fail("Error, priority queue size reported as %i != %i\n", s, N_THREADS);

  if(priority_queue_empty(&pq))
    fail("Error, priority queue is reported as empty despite containing %i threads\n", N_THREADS);

  /* Empty the unsorted priority queue. */
  for(i = 0; i < N_THREADS; i++)
  {
    thr = priority_queue_entry (priority_queue_max (&pq), struct thread, elem);
#ifdef DEBUG
    printf("Removing thread <%s>: priority %i wake_me_at %lli\n",
      thr->name, *thr->elem.p, *thr->elem.sort_val);
#endif
    if(thr->tid != i)
      fail("Error, max thread id is %i != %i\n", thr->tid, i);

    thr = priority_queue_entry (priority_queue_pop_front (&pq), struct thread, elem);
    expected_tid = i;
    if(thr->tid != expected_tid)
      fail("Error, popped thread id is %i != %i\n", thr->tid, expected_tid);

    size_t s = priority_queue_size(&pq);
    expected_size = N_THREADS - i - 1;
    if(s != expected_size)
      fail("Error, priority queue size reported as %i != %i\n", s, expected_size);

    priority_queue_verify (&pq, false, NULL, NULL);
  }

  if(!priority_queue_empty(&pq))
    fail("Error, priority queue is reported as non-empty despite having just been emptied\n", thr_ix);

  priority_queue_verify (&pq, false, NULL, NULL);

  /* Populate the priority queue, sorted. */
#ifdef DEBUG
  printf("Populating the priority queue, sorted\n");
#endif
  for(i = 0; i < N_THREADS; i++)
  {
    thr = &threads[i];

#ifdef DEBUG
    printf("Inserting thread <%s>: priority %i wake_me_at %lli\n",
      thr->name, *thr->elem.p, *thr->elem.sort_val);
#endif
    priority_queue_insert_ordered(&pq, &thr->elem, sleeping_list_less, NULL);
    expected_size = i + 1;
    if(pq.size != expected_size)
      fail("Error, pq size is %i != %i\n", pq.size, expected_size);

    priority_queue_verify (&pq, true, sleeping_list_less, sleeping_list_eq);
  }

  if(N_THREADS != thr_ix)
    fail("Internal error, N_THREADS %i != thr_ix %i\n", N_THREADS, thr_ix);

  s = priority_queue_size(&pq);
  if(s != N_THREADS)
    fail("Error, priority queue size reported as %i != %i\n", s, N_THREADS);

  if(priority_queue_empty(&pq))
    fail("Error, priority queue is reported as empty despite containing %i threads\n", N_THREADS);

  /* Empty the sorted priority queue. */
  int threads_removed = 0;
  for(i = 0; i < PRI_QUEUE_NLISTS; i++)
  {
    for(j = 0; j < THR_PER_PRI; j++)
    {
      thr = NULL;

      thr = priority_queue_entry (priority_queue_max (&pq), struct thread, elem);
#ifdef DEBUG
      printf("Removing thread <%s>: priority %i wake_me_at %lli\n",
        thr->name, *thr->elem.p, *thr->elem.sort_val);
#endif
      expected_tid = (THR_PER_PRI - j) + THR_PER_PRI*i - 1;
      if(thr->tid != expected_tid)
        fail("Error, max thread id is %i != %i\n", thr->tid, expected_tid);

      thr = priority_queue_entry (priority_queue_pop_front (&pq), struct thread, elem);
      expected_tid = (THR_PER_PRI - j) + THR_PER_PRI*i - 1;
      if(thr->tid != expected_tid)
        fail("Error, popped thread id is %i != %i\n", thr->tid, expected_tid);
      threads_removed++;

      size_t s = priority_queue_size(&pq);
      expected_size = N_THREADS - threads_removed;
      if(s != expected_size)
        fail("Error, priority queue size reported as %i != %i\n", s, expected_size);

      priority_queue_verify (&pq, true, sleeping_list_less, sleeping_list_eq);
    }
  }

  if(!priority_queue_empty(&pq))
    fail("Error, priority queue is reported as non-empty despite having just been emptied\n");

  /* List is empty, so both verification methods should work. */
  priority_queue_verify (&pq, false, NULL, NULL);
  priority_queue_verify (&pq, true, sleeping_list_less, sleeping_list_eq);

  pass ();
}
