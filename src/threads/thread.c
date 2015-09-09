#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Priority queue of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct priority_queue ready_queue;

/* Priority queue of sleeping processes, i.e. those waiting for enough time to pass 
   before they should be scheduled.
   Each list in the queue is sorted by wake_me_at, soonest to latest.
     Interpretation of sleeping_queue.val: 
       -1 means there are no sleeping threads.
        Else, val is the minimum wake_me_at value over all the lists
          (i.e. the time at which the next thread should be woken). 
          
  This list is shared by devices/timer.c and the waker_sleeping_threads.
  It should only be added to using push_sleeping_queue, which pushes in
  a wake_me_at-aware fashion. */
static struct priority_queue sleeping_queue;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */
/* System load average: est. of # threads ready to run over the past minute. */
static fp load_avg;

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.
   Also initializes the sleeping_queue and the associated 
     synchronization variables.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);

  priority_queue_init (&ready_queue);
  priority_queue_init (&sleeping_queue);

  list_init (&all_list);

  load_avg = int_to_fp (0);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  tid_t new_tid;

  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  new_tid = thread_create ("idle", PRI_MIN, idle, &idle_started);
  ASSERT (new_tid != TID_ERROR);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The new thread may be scheduled immediately if it has a higher
   priority than this thread. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;
  enum intr_level old_level;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue.
     If new thread has a higher priority than we do, yield.

     We do this with interrupts disabled to prevent yielding for no reason
     in the event of a timer interrupt that causes the newly-unblocked thread
     to run to completion before we go again.
     */
  old_level = intr_disable ();
  thread_unblock (t);
  if (thread_current ()->effective_priority < priority)
    thread_yield ();
  intr_set_level (old_level);

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  priority_queue_push_back (&ready_queue, &t->elem);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the status of the specified thread. */
enum thread_status
thread_get_status (struct thread *thr)
{
  ASSERT (thr != NULL);
  return thr->status;
}

/* Returns whether or not the specified thread is sleeping. */
bool
thread_is_sleeping (struct thread *thr)
{
  ASSERT (thr != NULL);
  /* No thread can request to be woken at time 0.
     No thread is started until after timer.c::ticks > 0, and timer_sleep
     returns on negative sleep requests. */
  return (0 < thr->wake_me_at);
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    priority_queue_push_back (&ready_queue, &cur->elem);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* ***************
   Functions for the priority scheduler. 
   *************** */

/* Returns the current thread's (effective) priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->effective_priority;
}

/* Sets the current thread's priority to NEW_PRIORITY. 
   If the current thread no longer has the highest priority, yields. */
void
thread_set_priority (priority new_priority) 
{
  ASSERT (PRI_MIN <= new_priority && new_priority <= PRI_MAX);

  enum intr_level old_level;
  priority orig_priority;

  struct thread *cur = thread_current ();

  /* Atomic operation so that no thread ever sees me with an inconsistent priority/effective priority. */
  old_level = intr_disable ();

  orig_priority = cur->effective_priority; 
  cur->base_priority = new_priority;
  cur->effective_priority = new_priority;

  /* Update my effective priority based on the locks I currently hold. */
  thread_return_priority ();

  /* If I have lowered my priority, yield. */
  if (cur->effective_priority < orig_priority)
    thread_yield ();

  intr_set_level (old_level);
}

/* Offers donor thread's priority to the recipient thread.
   Returns true if the priority is accepted, false if not
   needed. 
   This function only modifies recipient's effective
   priority, and does not modify the location of the recipient
   in its waiter list or priority queue.

   If recipient is in a priority queue, its location should be updated
   after this function returns. synch.c does not care about the order 
   of elements in its waiter lists.

   This function is intended to be called from thread_donate_priority.
   It must be called with interrupts turned off. */
bool
thread_offer_priority (struct thread *recipient, struct thread *donor)
{
  ASSERT (recipient != NULL);
  ASSERT (donor != NULL);
  ASSERT (intr_get_level () == INTR_OFF);

  if (has_lower_priority (recipient, donor))
  {
    recipient->effective_priority = donor->effective_priority;
    return true;
  }
  return false;
}

/* Thread donates its priority to the specified thread,
   and to anyone holding a lock that that thread is waiting on. 
   
   This function does up to MAX_DONATION_DEPTH levels of nested donation.
   
   TODO This function can detect trivial deadlocks if we want it to. That would be cool. */
void
thread_donate_priority (struct thread *recipient)
{
  int donation_depth;
  enum intr_level old_level;

  ASSERT (recipient != NULL);
  ASSERT (is_thread (recipient));

  /* Atomically modify priority levels of all affected threads. */
  old_level = intr_disable ();

  /* Who are we? */
  struct thread *cur = thread_current ();
  /* Who holds the lock we need? */
  struct thread *holder = recipient;

  /* Nested donation required?
     In the event of a deadlock, we don't want to donate forever, so
     donate up to MAX_DONATION_DEPTH times. */
  for (donation_depth = 0; donation_depth < MAX_DONATION_DEPTH; donation_depth++)
  {
    /* Figure out where the holder resides. One of several places:
        is_holder_ready == true                ==> ready_list
        is_holder_sleeping == true             ==> sleeping_list
        is_holder_otherwise_engaged == true    ==> on the waiters list of a sema or a cond */
    bool is_holder_ready = (thread_get_status (holder) == THREAD_READY);
    bool is_holder_sleeping = thread_is_sleeping (holder);
    bool is_holder_otherwise_engaged = ( ! (is_holder_ready || is_holder_sleeping) );
    ASSERT (is_holder_ready ^ is_holder_sleeping ^ is_holder_otherwise_engaged);

    if (thread_offer_priority (holder, cur))
    {
      /* Holder accepted our priority donation.
         If in ready or sleeping queue, we need to relocate it.

         If otherwise_engaged, simply changing its effective priority
         will make synch.c do the right thing: synch.c does not maintain
         sorted waiters lists. */

      /* Relocate holder in its current list or priority queue. */
      if (is_holder_ready || is_holder_sleeping)
      {
        struct priority_queue *pq = (is_holder_ready ? &ready_queue : &sleeping_queue);
        priority_queue_remove (pq, &holder->elem);
        priority_queue_push_back (pq, &holder->elem);
      }

      /* See if we need to offer our priority further. */
      if( holder->pending_lock != NULL )
        holder = holder->pending_lock->holder;
      else
        /* Holder is not waiting on anything, no more nested donation. */
        break;
    }
    else
      /* Holder rejects our offer (i.e. we are lower priority than holder), 
         nothing more to do. */
      break;

  } /* Up to MAX_DONATION_DEPTH donations. */

  intr_set_level (old_level);
}

/* Thread no longer holds a resource, and
   may need to "return" the increase in effective priority 
   it gained as a result of holding the resource. 
   Thread sets its effective priority to be MAX( base priority,
    effective priority of each thread waiting on the resources it holds). 

   In English, "return" is the inverse of "donate", but unlike 
   thread_donate_priority, here we are not modifying any threads 
   but ourselves.

   Returns true if the caller lowers its priority.
   If the caller lowers its priority, the caller should
   immediately yield to allow a potential higher-priority thread to run. */
bool
thread_return_priority (void)
{
  bool did_lower_priority = false;
  priority entry_priority, max_available_priority;
  enum intr_level old_level;

  struct thread *cur = thread_current ();

  /* Disable interrupts so that this operation is atomic. */
  old_level = intr_disable ();

  /* Where did we start? */
  entry_priority = cur->effective_priority;
  /* Restore to original priority level. */
  ASSERT (PRI_MIN <= cur->base_priority && cur->base_priority <= PRI_MAX);

  cur->effective_priority = cur->base_priority;

  /* Track priority level we should use. */
  max_available_priority = cur->effective_priority;

  /* For each lock we hold, determine the maximum priority waiter. */
  struct list_elem *e = NULL;
  for (e = list_begin (&cur->lock_list); e != list_end (&cur->lock_list);
       e = list_next (e))
    {
      /* If we are at the maximum level possible, no need to look further. */
      if (max_available_priority == PRI_MAX)
        break;

      struct lock *lock = list_entry (e, struct lock, elem);
      struct list_elem *max_prio_waiter = sema_max_waiter (&lock->semaphore);
      if (max_prio_waiter != NULL)
      {
        struct thread *waiter_thr = priority_queue_entry (list_entry (max_prio_waiter, struct priority_queue_elem, elem),
                                                          struct thread, elem);
        ASSERT (waiter_thr != NULL);
        ASSERT (is_thread (waiter_thr));
        if (max_available_priority < waiter_thr->effective_priority)
          max_available_priority = waiter_thr->effective_priority;
      }
    }

  ASSERT (PRI_MIN <= max_available_priority && max_available_priority <= PRI_MAX);

  /* Set our priority to the maximum priority we found. */
  cur->effective_priority = max_available_priority;

  if (cur->effective_priority < entry_priority)
    did_lower_priority = true;

  intr_set_level (old_level);
  return did_lower_priority;
}

/* Returns true if a has lower effective priority than b, and false else. */
bool
has_lower_priority (struct thread *a, struct thread *b)
{
  ASSERT (a != NULL);
  ASSERT (is_thread (a));
  ASSERT (b != NULL);
  ASSERT (is_thread (b));

  return (a->effective_priority < b->effective_priority);
}



/* ***************
   Functions for the BSD scheduler. 
   *************** */

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  /* Not yet implemented. */
  return 0;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) 
{
  /* Not yet implemented. */
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  /* Not yet implemented. */
  return 0;
}

/* Returns 100 times the last computed system load average. */
int
thread_get_load_avg (void) 
{
  return 100*fp_to_int( load_avg, ROUND_TO_NEAREST );
}

/* Calculate the system load average, and update thread.::load_avg.
   Must be called on a TIMER_FREQ boundary.
   Intended for use by devices/timer.c. */
fp
thread_calc_load_avg (void)
{
  ASSERT ( timer_ticks () % TIMER_FREQ == 0 );
  /* Compute coefficients. */
  fp A = fp_fp_div (int_to_fp (59), int_to_fp (60)); /* 59/60 */
  fp B = fp_fp_div (int_to_fp (1), int_to_fp (60)); /* 1/60 */
  /* load_avg = A*load_avg + B*ready_threads */
  load_avg = fp_fp_add (fp_fp_mult (A, load_avg), fp_fp_mult (B, load_avg));
  return load_avg;
}

/* ***************
   Functions for the alarm clock. 
   *************** */

/* API to allow timer.c::timer_sleep to add threads to the sleeping_queue.
   Threads are inserted into the list of the appropriate priority.
   Lists are kept sorted by wake_me_at. */
void
push_sleeping_queue (struct thread *t)
{
  ASSERT (t != NULL);
  priority_queue_insert_ordered (&sleeping_queue, &t->elem, sleeping_queue_less, NULL);
  /* Check if we are now the soonest-to-wake. */
  if(t->wake_me_at <= sleeping_queue.val || sleeping_queue.val == -1)
    sleeping_queue.val = t->wake_me_at;
}

/* Input: The list_elem of two priority queue elements
   in the sleeping_queue, with sort_val set to the wake_me_at 
   value of the threads.
   Returns true if a < b (a should wake before b), false if a >= b. */
bool
sleeping_queue_less (const struct list_elem *a,
                     const struct list_elem *b,
                     void *aux UNUSED)
{
  ASSERT (a != NULL);
  ASSERT (b != NULL);

  struct priority_queue_elem *a_elem = list_entry (a, struct priority_queue_elem, elem);
  struct priority_queue_elem *b_elem = list_entry (b, struct priority_queue_elem, elem);

  return (*a_elem->sort_val < *b_elem->sort_val);
}

/* Input: The list_elem of two priority queue elements
   in the sleeping_queue, with sort_val set to the wake_me_at 
   value of the threads.

   Output: true if A == B, false if A != B */
bool
sleeping_queue_eq (const struct list_elem *a,
                  const struct list_elem *b,
                  void *aux UNUSED)
{
  ASSERT (a != NULL);
  ASSERT (b != NULL);

  struct priority_queue_elem *a_elem = list_entry (a, struct priority_queue_elem, elem);
  struct priority_queue_elem *b_elem = list_entry (b, struct priority_queue_elem, elem);

  return (*a_elem->sort_val == *b_elem->sort_val);
}


/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Waker logic. Requested by the interrupt handler to wake sleeping threads 
   found on the sleeper_list. 
   
   Must be called with interrupts disabled, or from an interrupt context.
   */
void
wake_sleeping_threads (int64_t now)
{
  ASSERT (intr_context() || intr_get_level () == INTR_OFF);
  ASSERT (0 <= now);
  /* If there are no threads sleeping at all, or if no sleepers
     are ready to wake, then there is nothing to do. */
  if (sleeping_queue.val == -1 || now < sleeping_queue.val)
    return;

  /* We should only have been woken and gotten here if there is at least 
     one thread to wake. */
  ASSERT (0 <= sleeping_queue.val && sleeping_queue.val <= now);
  /* As we are about to make sleeping_queue.val (aka time_for_next_wake) 
     invalid, track the next soonest to wake thread out of those we 
     do not wake here. */
  int64_t next_soonest_to_wake = -1;

  struct list *l = NULL;
  struct thread *t = NULL;
  struct list_elem *e = NULL;
  priority pri_lvl = -1;
  /* Did we find a wakeable thread this iter? */
  bool found_wakeable_thread = false;

  /* Wake threads in priority order. */
  for (pri_lvl = PRI_MAX-1; pri_lvl >= 0; pri_lvl--)
    {
      /* Each list is sorted by wake_me_at time, smallest to largest. */
      l = &sleeping_queue.queue[pri_lvl];

      e = list_begin (l);
      while (e != list_end (l))
        {
          t = priority_queue_entry (list_entry (e, struct priority_queue_elem, elem), 
                                    struct thread, elem);
          ASSERT (0 <= t->wake_me_at);
          /* Is this thread ready to be woken? */
          if (t->wake_me_at <= now)
            {
              found_wakeable_thread = true;
              /* See timer_sleep: threads in l may have 
                 status THREAD_RUNNING until thread_block() is called. 
                 Until thread status is THREAD_BLOCKED, we cannot safely 
                 move the thread to the ready list. */
              if (t->status == THREAD_BLOCKED)
                {
                  /* Wake up the thread. */
                  t->status = THREAD_READY;
                  t->wake_me_at = 0;
                  /* Remove and advance to next element. */
                  e = priority_queue_remove (&sleeping_queue, list_entry (e, struct priority_queue_elem, elem));

#ifndef NDEBUG
                  priority_queue_verify (&ready_queue, false, NULL, NULL);
#endif

                  /* Atomically add to ready_queue.
                     We disable interrupts here rather than outside the loop to avoid
                     keeping interrupts disabled for too long. We assume that there are many threads
                     waiting and that most of them do not need to be woken. */
                  priority_queue_push_back (&ready_queue, &t->elem);

#ifndef NDEBUG
                  priority_queue_verify (&sleeping_queue, true, sleeping_queue_less, sleeping_queue_eq);
#endif

                } /* End of status == THREAD_BLOCKED. */
              else
                {
                  e = e->next;
                }
            } /* End of wake_me_at <= now */
            else
              {
                /* This is the first element in the list that should not be woken. Check if it is the soonest to wake of the unwoken threads. */
                if(next_soonest_to_wake == -1 || t->wake_me_at < next_soonest_to_wake)
                  next_soonest_to_wake = t->wake_me_at;
                break;
              }
        } /* Loop over threads at THIS priority level. */
    } /* Loop over priority levels. */

    ASSERT (found_wakeable_thread == true);

    /* Set when we will next need to be woken by timer_interrupt. */
    sleeping_queue.val = next_soonest_to_wake;
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->base_priority = priority;
  t->effective_priority = priority;
  t->elem.p = &t->effective_priority;
  list_init (&t->lock_list);
  t->pending_lock = NULL;
  t->wake_me_at = 0;
  t->elem.sort_val = NULL;
  t->magic = THREAD_MAGIC;
  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  struct thread *thr = NULL;

#ifndef NDEBUG
  priority_queue_verify (&ready_queue, false, NULL, NULL);
#endif

  if (priority_queue_empty (&ready_queue))
    thr = idle_thread;
  else
    thr = priority_queue_entry (priority_queue_pop_front (&ready_queue), 
                           struct thread, elem);
  ASSERT (is_thread (thr));

#ifndef NDEBUG
  priority_queue_verify (&ready_queue, false, NULL, NULL);
#endif

  return thr;

  /* TODO restore this
  if (priority_queue_empty (&ready_queue))
    return idle_thread;
  else
    return priority_queue_pop_front (&ready_queue);
    */
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
