#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "lib/kernel/fpra.h"
#include "threads/resource.h"

/* States in a thread's life cycle. */
enum thread_status
  {
    THREAD_RUNNING,     /* Running thread. */
    THREAD_READY,       /* Not running but ready to run. */
    THREAD_BLOCKED,     /* Waiting for an event to trigger. */
    THREAD_DYING        /* About to be destroyed. */
  };

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

#define MAX_DONATION_DEPTH 8 /* Don't donate priority more than 8 levels. */
typedef int priority;

/* A kernel thread or user process.

   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+

   The upshot of this is twofold:

      1. First, `struct thread' must not be allowed to grow too
         big.  If it does, then there will not be enough room for
         the kernel stack.  Our base `struct thread' is only a
         few bytes in size.  It probably should stay well under 1
         kB.

      2. Second, kernel stacks must not be allowed to grow too
         large.  If a stack overflows, it will corrupt the thread
         state.  Thus, kernel functions should not allocate large
         structures or arrays as non-static local variables.  Use
         dynamic allocation with malloc() or palloc_get_page()
         instead.

   The first symptom of either of these problems will probably be
   an assertion failure in thread_current(), which checks that
   the `magic' member of the running thread's `struct thread' is
   set to THREAD_MAGIC.  Stack overflow will normally change this
   value, triggering the assertion. */
/* A thread is assigned a priority at creation time. 
   When using the priority scheduler (this is the default scheduling policy), 
   the thread's priority may be temporarily raised due to a donation
   from a higher-priority thread waiting on a resource held by the 
   thread. We track this using the 'priority' member (thread's base priority)
   and the 'effective_priority' member (thread's current priority).
   For scheduling decisions, always use the effective_priority value. */
/* The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive: only a thread in the
   ready state is on the run queue, whereas only a thread in the
   blocked state is on a semaphore wait list. */
struct thread
  {
    /* Owned by thread.c. */
    tid_t tid;                          /* Thread identifier. */
    enum thread_status status;          /* Thread state. */
    char name[16];                      /* Name (for debugging purposes). */
    uint8_t *stack;                     /* Saved stack pointer. */
    priority base_priority;                  /* Base priority. */
    priority effective_priority;        /* Effective priority (donated?). */
    struct list_elem allelem;           /* List element for all threads list. */

    /* Shared between thread.c and synch.c. */
    struct list_elem elem;              /* List element. */

    /* Tracks resources owned by this thread, enabling this thread to correctly 
       update its effective priority after yielding a resource (and any donated
       priority resulting therefrom). 
       This list should only be used for resources that can have at most one 
       owner, e.g. synch.h::lock */
    struct list resource_list; /* One-owner resources we hold. */
    /* Tracks resource for which this thread is waiting, enabling
       nested priority donation. */
    struct resource *pending_resource; /* Resource we are waiting for. */

#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;                  /* Page directory. */
#endif

    /* Owned by devices/timer.c. */
    int64_t wake_me_at; /* Wake a thread in thread_status THREAD_BLOCKED when 0 < wake_me_at <= current timer tick */

    /* Owned by thread.c. */
    unsigned magic;                     /* Detects stack overflow. */
  };

/* A priority_queue is an abstract data type masking the implementation of 
   a queue of threads used for priority scheduling. */ 
#define PRI_QUEUE_NLISTS (1 + PRI_MAX - PRI_MIN)
struct priority_queue
  {
    struct list queue[PRI_QUEUE_NLISTS];
    size_t size; /* Total number of elements in all lists. */
    /* Useful for the sleeping_list. Could be handy in other places.
       In the sleeping list we track the minimum wake_me_at value. */  
    int64_t val; 
  };

/* TODO Could return list_elem and accept pairs of list_elem and priority.
   More completely, we would define an ADT in list.h for list_of_lists.
   Seems cleaner, but is it just abstraction for abstraction's sake? 

   TODO Write a priority_queue_remove function to
    eliminate the pq->size-- bug. */
void priority_queue_init (struct priority_queue *);
void priority_queue_verify (struct priority_queue *);
bool priority_queue_empty (struct priority_queue *);
size_t priority_queue_size (struct priority_queue *);
struct thread * priority_queue_pop_front (struct priority_queue *);
void priority_queue_push_back (struct priority_queue *, struct thread *);
void priority_queue_insert_ordered (struct priority_queue *, struct thread *, list_less_func *, void *aux);
struct thread * priority_queue_max (struct priority_queue *);

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);

int thread_get_priority (void);
void thread_set_priority (priority);
bool thread_offer_priority (struct thread *recipient, struct thread *donor, struct priority_queue *);

void thread_donate_priority (struct resource *);
bool thread_return_priority (void);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);
fp thread_calc_load_avg (void);

/* Functions to expose the variables used to support alarm clock functionality to devices/timer.c. */
void wake_sleeping_threads (int64_t);
void push_sleeping_list (struct thread*);
bool sleeping_list_less(const struct list_elem *a,
                        const struct list_elem *b,
                        void *aux UNUSED);

#endif /* threads/thread.h */
