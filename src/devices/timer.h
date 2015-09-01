#ifndef DEVICES_TIMER_H
#define DEVICES_TIMER_H

#include <round.h>
#include <stdint.h>

/* Number of timer interrupts per second. */
#define TIMER_FREQ 100

void timer_init (void);
void timer_calibrate (void);

int64_t timer_ticks (void);
int64_t timer_elapsed (int64_t);

/* Sleep and yield the CPU to other threads. */
void timer_sleep (int64_t ticks);
void timer_msleep (int64_t milliseconds);
void timer_usleep (int64_t microseconds);
void timer_nsleep (int64_t nanoseconds);

/* Busy waits. */
void timer_mdelay (int64_t milliseconds);
void timer_udelay (int64_t microseconds);
void timer_ndelay (int64_t nanoseconds);

void timer_print_stats (void);

/* Variables from thread.c */

/* List of sleeping processes, i.e. those waiting for enough time to pass 
   before they should be scheduled. */
extern struct list sleeping_list;
/* Lock to control access to sleeping_list. */
extern struct lock sleeping_list_lock;
/* Semaphore to signal that the timer interrupt handler ran. 
   Used by timer_interrupt and the waker thread. */
extern struct semaphore timer_interrupt_occurred;
/* What time was it when the timer interrupt handler went off? */
extern int64_t timer_interrupt_ticks;

#endif /* devices/timer.h */
