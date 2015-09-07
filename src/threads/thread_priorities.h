#ifndef THREADS_THREAD_PRIORITIES_H
#define THREADS_THREAD_PRIORITIES_H

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

/* Returns true if THREAD_A_P (pointer to a struct thread)
    has a lower priority than THREAD_B_P (pointer to a struct thread). */ 
#define HAS_LOWER_PRIORITY(THREAD_A_P, THREAD_B_P)      \
        ((THREAD_A_P)->effective_priority < (THREAD_B_P)->effective_priority) \

#endif /* threads/thread_priorities.h */
