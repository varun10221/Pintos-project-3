#ifndef THREADS_SYNCH_H
#define THREADS_SYNCH_H

#include <list.h>
#include <stdbool.h>
#include "threads/resource.h"
#include "threads/thread.h"

struct priority_queue;

/* A counting semaphore. */
struct semaphore 
  {
    unsigned value;             /* Current value. */
    struct priority_queue waiters;        /* List of waiting threads. */
  };

void sema_init (struct semaphore *, unsigned value);
/* Functions for vanilla semaphore usage. */
void sema_down (struct semaphore *);
bool sema_try_down (struct semaphore *);
void sema_up (struct semaphore *);
/* Functions for higher-level synchronization tools
   with one-at-at-time semantics built on top of semaphores. */
void sema_down_prio (struct semaphore *, struct resource *);
bool sema_try_down_prio (struct semaphore *, struct resource *);
void sema_up_prio (struct semaphore *, struct resource *);

void sema_self_test (void);

/* Lock. */
struct lock 
  {
    /* Begin with the elements of a resource, allowing us to use 
       a Lock as a more abstract resource. */
    struct thread *holder;      /* Thread holding lock. */
    struct list_elem elem;      /* List element for priority donation. */
    struct priority_queue *waiters;       /* List of waiting threads. */

    struct semaphore semaphore; /* Binary semaphore controlling access. */
  };

void lock_init (struct lock *);
void lock_acquire (struct lock *);
bool lock_try_acquire (struct lock *);
void lock_release (struct lock *);
bool lock_held_by_current_thread (const struct lock *);

/* Condition variable. */
struct condition 
  {
    struct list waiters; /* List of waiting threads. */
  };

void cond_init (struct condition *);
void cond_wait (struct condition *, struct lock *);
void cond_signal (struct condition *, struct lock *);
void cond_broadcast (struct condition *, struct lock *);

/* Optimization barrier.

   The compiler will not reorder operations across an
   optimization barrier.  See "Optimization Barriers" in the
   reference guide for more information.*/
#define barrier() asm volatile ("" : : : "memory")

#endif /* threads/synch.h */
