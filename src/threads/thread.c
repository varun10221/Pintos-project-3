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
static struct priority_queue ready_list;

/* Priority queue of sleeping processes, i.e. those waiting for enough time to pass 
   before they should be scheduled.
   Each list in the queue is sorted by wake_me_at, soonest to latest.
     Interpretation of sleeping_list.val: 
       -1 means there are no sleeping threads.
        Else, val is the minimum wake_me_at value over all the lists
          (i.e. the time at which the next thread should be woken). 
          
  This list is shared by devices/timer.c and the waker_sleeping_threads.
  It should only be added to using push_sleeping_list, which pushes in
  a wake_me_at-aware fashion. */
static struct priority_queue sleeping_list;

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

/* Initializes PRIORITY_QUEUE as an empty priority queue. */
void
priority_queue_init(struct priority_queue *pq)
{
  ASSERT (pq != NULL);

  int i;
  for (i = PRI_QUEUE_NLISTS-1; i >= 0; i--)
  {
    list_init (&pq->queue[i]);
  }
  pq->size = 0;
  pq->val = -1;
}

/* Verify that this priority queue is internally consistent. 
     1. Size is correct.
     2. Each element is in the correct list. 
   For debugging only.
   */
void
priority_queue_verify(struct priority_queue *pq)
{
  ASSERT (pq != NULL);

  struct list_elem *e = NULL;
  struct list *l = NULL;
  struct thread *t = NULL;

  size_t n_elts = 0;

  int i;
  for (i = PRI_QUEUE_NLISTS-1; i >= 0; i--)
  {
    l = &pq->queue[i];
    for (e = list_begin (l); e != list_end (l);
         e = list_next (e))
      {
        n_elts++;
        t = list_entry (e, struct thread, elem);
        ASSERT (t->effective_priority == i);
      }
  }

  ASSERT (n_elts == pq->size);
}

/* Returns true if empty, false else. */
bool
priority_queue_empty(struct priority_queue *pq)
{
  ASSERT(pq != NULL);
  return pq->size == 0;
}

/* Return size of this priority queue.
   Runs in O(N). */
size_t
priority_queue_size (struct priority_queue *pq)
{
  ASSERT (pq != NULL);
  return pq->size;
}

/* Returns the highest priority thread in the queue (round-robin in the event
   of a tie), or NULL if the queue is empty. */
struct thread *
priority_queue_pop_front(struct priority_queue *pq)
{
  ASSERT (pq != NULL);

  int i;
  struct thread *ret = NULL;
  for (i = PRI_QUEUE_NLISTS-1; i >= 0; i--)
  {
    struct list *l = &pq->queue[i];
    if (!list_empty (l))
    {
      pq->size--;  
      ret = list_entry (list_pop_front (l), struct thread, elem);
      /* Must be a thread. */
      ASSERT (is_thread (ret));
      /* Threads must be in the list matching their effective priority. */
      ASSERT (ret->effective_priority == i);
      break;
    }
  }
  if(ret != NULL)
  {
    ASSERT (is_thread (ret));
  }
  return ret;
}

/* Add this thread to the priority queue based on its effective priority. */
void
priority_queue_push_back(struct priority_queue *pq, struct thread *t)
{
  ASSERT (pq != NULL);
  ASSERT (t != NULL);
  ASSERT (is_thread (t));

  priority p = t->effective_priority;
  ASSERT (PRI_MIN <= p && p <= PRI_MAX);
  pq->size++;  
  list_push_back (&pq->queue[p], &t->elem);
}

/* Insert into this priority queue. */
void
priority_queue_insert_ordered (struct priority_queue *pq, struct thread *t, list_less_func *f, void *aux UNUSED)
{
  ASSERT (pq != NULL);
  ASSERT (t != NULL);
  ASSERT (is_thread (t));
  ASSERT (f != NULL);

  priority p = t->effective_priority;
  ASSERT (PRI_MIN <= p && p <= PRI_MAX);
  pq->size++;  
  list_insert_ordered (&pq->queue[p], &t->elem, f, NULL);
}

/* Identify the maximum priority thread in the priority_queue. 
   Returns NULL if queue is empty. 
   Does not remove the thread, just returns a pointer to it. */
struct thread *
priority_queue_max(struct priority_queue *pq)
{
  ASSERT (pq != NULL);

  int i;
  struct thread *thr = NULL;
  for (i = PRI_QUEUE_NLISTS-1; i >= 0; i--)
  {
    struct list *l = &pq->queue[i];
    if (!list_empty (l))
    {
      thr = list_entry (list_front (l), struct thread, elem);
      ASSERT (is_thread (thr));
      return thr;
    }
  }
  return NULL;
}

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.
   Also initializes the sleeping_list and the associated 
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

  priority_queue_init (&ready_list);
  priority_queue_init (&sleeping_list);

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
  priority_queue_push_back (&ready_list, t);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
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
    priority_queue_push_back (&ready_list, cur);
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

  struct thread *t = thread_current ();
  priority orig_priority = t->effective_priority;

  /* Atomic operation so that no thread ever sees me with an inconsistent priority/effective priority. */
  old_level = intr_disable ();
  t->base_priority = new_priority;
  t->effective_priority = new_priority;

  /* Update my effective priority based on resources I hold. */
  thread_return_priority ();

  /* If I just lowered my priority, yield. */
  if(orig_priority != t->effective_priority)
    thread_yield ();
  intr_set_level (old_level);
}

/* Offers donor thread's priority to the recipient thread.
   Returns true if the priority is accepted, false if not
   needed. This function only modifies recipient's effective
   priority, and does not perform nested priority donation. 

   If recipient accepts the priority, updates the location of recipient
   in its priority queue.
  
   This function must be called with interrupts turned off.
   */
bool
thread_offer_priority (struct thread *recipient, struct thread *donor, struct priority_queue *pq)
{
  ASSERT (recipient != NULL);
  ASSERT (donor != NULL);
  ASSERT (pq != NULL);
  ASSERT (intr_get_level () == INTR_OFF);

  if(recipient->effective_priority < donor->effective_priority)
  {
    recipient->effective_priority = donor->effective_priority;
    /* Relocate recipient in its priority queue. */
    pq->size--; /* Don't double-count this thread in pq. */
    list_remove (&recipient->elem);
    priority_queue_push_back (pq, recipient);
    /* TODO DEBUG CODE */
    priority_queue_verify(pq);
    return true;
  }
  return false;
}

/* Thread donates its priority to the holder of the specified resource, and 
   to anyone that holder is waiting on. This function does up to 
   MAX_DONATION_DEPTH levels of donation. */
void
thread_donate_priority (struct resource *res)
{
  int donation_depth;
  enum intr_level old_level;

  ASSERT (res != NULL);

  old_level = intr_disable ();

  /* Verify that this thread is waiting on res. */
  struct thread *t = thread_current ();
  ASSERT ( t->pending_resource == res );

  /* Who holds res? */
  struct thread *holder = res->holder;
  ASSERT (holder != NULL);

  /* Where is the holder thread? */
  struct priority_queue *holder_priority_queue = NULL;
  if (holder->pending_resource == NULL)
    holder_priority_queue = &ready_list;
  else
    holder_priority_queue = holder->pending_resource->waiters;

  /* Nested donation required?
     In the event of a deadlock, we don't want to donate forever, so
     donate up to MAX_DONATION_DEPTH times. */
  for(donation_depth = 0; donation_depth < MAX_DONATION_DEPTH; donation_depth++)
  {
    if( thread_offer_priority (holder, t, holder_priority_queue) )
    {
      /* Holder accepted our priority. See if we need to pass it on. */
      if( holder->pending_resource != NULL )
      {
        holder = holder->pending_resource->holder;
        /* Where is the holder thread? */
        if (holder->pending_resource == NULL)
          holder_priority_queue = &ready_list;
        else
          holder_priority_queue = holder->pending_resource->waiters;
      }
      else
        /* Holder is not waiting on anything, no nested donation. */
        break;
    }
    else
      /* Holder rejects our offer (i.e. we are lower priority than holder), 
         nothing more to do. */
      break;
  }
  intr_set_level (old_level);
  return;
}

/* Thread no longer holds a resource, and
   may need to "return" the increase in effective priority 
   it gained as a result of holding the resource. 
   Thread sets its effective priority to be MAX( base priority,
    effective priority of each thread waiting on the resources it holds ). 

   Returns true if it lowers its priority as a result. */
bool
thread_return_priority (void)
{
  bool lowered_priority = false;
  priority entry_priority = PRI_MAX;
  enum intr_level old_level;

  /* Disable interrupts so that we aren't surprised by other threads
     changing their priorities. */
  old_level = intr_disable ();
  struct thread *t = thread_current ();

  /* Restore to original priority level. */
  ASSERT (PRI_MIN <= t->base_priority && t->base_priority <= PRI_MAX);
  entry_priority = t->effective_priority;
  t->effective_priority = t->base_priority;

  /* Track priority level we should use. */
  priority max_available_priority = t->effective_priority;

  /* For each resource we hold, determine the maximum priority waiter. */
  struct list_elem *e = NULL;
  for (e = list_begin (&t->resource_list); e != list_end (&t->resource_list);
       e = list_next (e))
    {
      /* If we are at the maximum level possible, no need to look further. */
      if (max_available_priority == PRI_MAX)
        break;

      struct resource *res = list_entry (e, struct resource, elem);
      struct thread *max = priority_queue_max (res->waiters);
      if(max != NULL && max_available_priority < max->effective_priority )
        max_available_priority = max->effective_priority;
    }

  /* If we found a higher priority waiter, raise our priority. */
  if (t->effective_priority < max_available_priority)
    t->effective_priority = max_available_priority;
  ASSERT (PRI_MIN <= t->effective_priority && t->effective_priority <= PRI_MAX);

  if (t->effective_priority < entry_priority)
    lowered_priority = true;

  intr_set_level (old_level);
  return lowered_priority;
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

/* API to allow timer.c::timer_sleep to add threads to the sleeping_list.
   Threads are inserted into the list of the appropriate priority.
   Lists are kept sorted by wake_me_at. */
void
push_sleeping_list (struct thread *t)
{
  ASSERT (t != NULL);
  priority_queue_insert_ordered (&sleeping_list, t, sleeping_list_less, NULL);
  /* Check if we are now the soonest-to-wake. */
  if(t->wake_me_at <= sleeping_list.val || sleeping_list.val == -1)
    sleeping_list.val = t->wake_me_at;
}

/* Input: two list elements belonging to threads.
   Output: true if A < B, false if A >= B
   Uses the wake_me_at value for comparison. */
bool
sleeping_list_less(const struct list_elem *a,
                        const struct list_elem *b,
                        void *aux UNUSED)
{
  ASSERT (a != NULL);
  ASSERT (b != NULL);

  struct thread *a_thr = list_entry(a, struct thread, elem);
  struct thread *b_thr = list_entry(b, struct thread, elem);
  ASSERT (is_thread (a_thr));
  ASSERT (is_thread (b_thr));

  return (a_thr->wake_me_at <= b_thr->wake_me_at);
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
  ASSERT (0 <= now);
  /* If there are no threads sleeping at all, or if no sleepers
     are ready to wake, then there is nothing to do. */
  if (sleeping_list.val == -1 || now < sleeping_list.val)
    return;

  /* We should only have been woken and gotten here if there is at least 
     one thread to wake. */
  ASSERT (0 <= sleeping_list.val && sleeping_list.val <= now);
  /* As we are about to make sleeping_list.val (aka time_for_next_wake) 
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
      l = &sleeping_list.queue[pri_lvl];

      e = list_begin (l);
      while (e != list_end (l))
        {
          t = list_entry (e, struct thread, elem);
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
                  e = list_remove (e);
                  sleeping_list.size--;

                  /* TODO DEBUG CODE */
                  priority_queue_verify(&ready_list);

                  /* Atomically add to ready_list.
                     We disable interrupts here rather than outside the loop to avoid
                     keeping interrupts disabled for too long. We assume that there are many threads
                     waiting and that most of them do not need to be woken. */
                  priority_queue_push_back (&ready_list, t);

                  /* TODO DEBUG CODE */
                  priority_queue_verify(&sleeping_list);

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
    sleeping_list.val = next_soonest_to_wake;
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
  list_init (&t->resource_list);
  t->pending_resource = NULL;
  t->wake_me_at = 0;
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

  /* TODO DEBUG CODE */
  priority_queue_verify(&ready_list);

  if (priority_queue_empty (&ready_list))
    thr = idle_thread;
  else
    thr = priority_queue_pop_front (&ready_list);
  ASSERT (is_thread (thr));

  /* TODO DEBUG CODE */
  priority_queue_verify(&ready_list);

  return thr;

  /* TODO restore this
  if (priority_queue_empty (&ready_list))
    return idle_thread;
  else
    return priority_queue_pop_front (&ready_list);
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
