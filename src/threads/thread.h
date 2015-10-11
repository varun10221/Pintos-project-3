#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <vector.h>
#include <priority_queue.h>
#include <stdint.h>
#include "lib/kernel/fpra.h"
#include "threads/thread_priorities.h"
#include "vm/page.h"

/* Forward declarations. */
struct lock;
struct child_process_info;

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

#define MAX_DONATION_DEPTH 16 /* Don't donate priority more than 16 levels. */

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
   from a higher-priority thread waiting on a lock held by the 
   thread. We track this using the 'priority' member (thread's base priority)
   and the 'effective_priority' member (thread's current priority).
   For scheduling decisions, always use the effective_priority value. 
   thread_set_priority modifies the base priority and potentially the effective
   priority.  */
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
    priority base_priority;             /* Base priority. */
    /* effective_priority should be used for any scheduling decisions. 
       It may differ from base_priority in the event of priority donation.
       When using the mlfqs, base_priority is ignored. */
    priority effective_priority;        /* Effective priority */
    int nice; /* The 'nice' value in the mlfqs. */
    fp recent_cpu; /* The 'recent cpu' value in the mlfqs. */
    struct list_elem allelem;    /* List element for all threads list. */
    bool in_time_slice_list;     /* Whether or not we are in the time_slice_threads list. */
    struct list_elem ts_elem;    /* List element for time_slice_threads list. */

    /* The following variables are shared between thread.c and synch.c. */
    struct priority_queue_elem elem;    /* Priority queue element. */

    /* Tracks locks held by this thread.
       Enables this thread to correctly update its effective priority after 
       yielding a resource (and any donated priority resulting therefrom). 

       This list should only be used for resources that can have at most one 
       owner, e.g. synch.h::lock */
    struct list lock_list; /* List of locks we hold. */

    /* Tracks the lock for which this thread is waiting, enabling
       nested priority donation. */
    struct lock *pending_lock; /* Lock we are blocked on. */

#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;                  /* Page directory. */
    /* Owned by userprog/process.c. 
       Maps fd's to struct file *s. */
    struct vector fd_table;
    /* Owned by userprog/process.c. Used to keep the
       executable from being modified. */
    struct file *my_executable;

    /* Owned by userprog/process.c. */
    struct list child_list; /* List of direct children. */
    struct child_process_info *child_info_self; /* Pointer to my entry in my parent's child_list. */
#endif

    /* Structures for virtual memory. */

    /* Maps mmap_id_t to struct mmap_info*. */
    struct vector mmap_table;
    /* Used to map virtual addresses to pages. */
    struct supp_page_table supp_page_table;

    /* Owned by devices/timer.c. */
    int64_t wake_me_at; /* Wake a thread in thread_status THREAD_BLOCKED when 0 < wake_me_at <= current timer tick */

    /* Owned by thread.c. */
    unsigned magic;                     /* Detects stack overflow. */
  };

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

/* Functions to query info about a thread. */
struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);
enum thread_status thread_get_status (struct thread *);
bool thread_is_sleeping (struct thread *);
struct child_process_info * thread_get_child_info_self (void);
void thread_set_child_info_self (struct child_process_info *cpi);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);

/* Priority-themed functions. */
int thread_get_priority (void);
void thread_set_priority (priority);

bool thread_offer_priority (struct thread *recipient, struct thread *donor);
void thread_donate_priority (struct thread *);
bool thread_return_priority (void);

/* Returns true if THREAD_A_P (pointer to a struct thread)
    has a lower priority than THREAD_B_P (pointer to a struct thread). */ 
bool has_lower_priority (struct thread *a, struct thread *b);

bool waiter_list_less(const struct list_elem *a,
                      const struct list_elem *b,
                      void *aux UNUSED);

/* Functions for mlfqs. */
int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);
void thread_calc_load_avg (void);

void update_threads_priority (void);
void thread_update_priority (struct thread *, void *);
void update_threads_recent_cpu (void);
void thread_update_recent_cpu (struct thread *, void *);

void update_recent_threads_priority (void);
void empty_recent_threads_list (void);

/* Functions to expose the variables used to support alarm clock functionality to devices/timer.c. */
void wake_sleeping_threads (int64_t);
void push_sleeping_queue (struct thread*);
bool sleeping_queue_less(const struct list_elem *a,
                        const struct list_elem *b,
                        void *aux UNUSED);
bool sleeping_queue_eq (const struct list_elem *a,
                       const struct list_elem *b,
                       void *aux UNUSED);

/* Functions for interacting with a thread's held locks. */
void thread_release_all_locks (void);

#endif /* threads/thread.h */
