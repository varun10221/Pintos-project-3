#ifndef __LIB_KERNEL_PRIORITY_QUEUE_H
#define __LIB_KERNEL_PRIORITY_QUEUE_H

#include <list.h>
#include "threads/thread_priorities.h"

/* Priority queue. */

/* A priority_queue is an abstract data type consisting of an array of lists
   ordered by some priority level and (optionally) with the list corresponding
   to each priority level sorted in a user-defined fashion.
   Default is to have each list be FIFO.
    
   Each structure that is a potential priority_queue element must embed a 
   struct priority_queue_elem member.  All of the list functions operate on these 
   `struct priority_queue_elem's. The priority_queue_entry macro allows 
   conversion from a struct priority_queue_elem back to a structure object 
   that contains it. 
   
   The size of a priority queue is fixed to be the number of different 
   priorities that a thread can have. */

#define PRI_QUEUE_NLISTS (1 + PRI_MAX - PRI_MIN)

/* Mask what a priority is. */
typedef int64_t priority;

struct priority_queue_elem
  {
    struct list_elem elem; /* List element. */
    priority *p;           /* Priority of the element. */
    int64_t *sort_val;     /* Sort by this value if desired. */
  };

#define PRI_QUEUE_NLISTS (1 + PRI_MAX - PRI_MIN)
struct priority_queue
  {
    struct list queue[PRI_QUEUE_NLISTS];
    size_t size; /* Total number of elements in all lists. */
    /* Useful for the sleeping_list. Could be handy in other places.
       In the sleeping list we track the minimum wake_me_at value. */  
    int64_t val; 
  };

/* Converts pointer to priority_queue element PRIORITY_QUEUE_ELEM into a 
   pointer to the structure that PRIORITY_QUEUE_ELEM is embedded inside.
   Supply the name of the outer structure STRUCT and the member name MEMBER
   of the priority_queue element. Modeled after list_entry 
   from lib/kernel/list.h. */
/* TODO Does this work?? */
#define priority_queue_entry(PRIORITY_QUEUE_ELEM, STRUCT, MEMBER)           \
        ((STRUCT *) ((uint8_t *) &(PRIORITY_QUEUE_ELEM)->elem               \
                     - offsetof (STRUCT, MEMBER.elem)))

/* Functions in the style of list. */
void priority_queue_init (struct priority_queue *);

/* PQ insertion. */
void priority_queue_push_back (struct priority_queue *, struct priority_queue_elem *);
void priority_queue_insert_ordered (struct priority_queue *, struct priority_queue_elem *, list_less_func *, void *aux);

/* PQ removal. */
struct priority_queue_elem *priority_queue_pop_front (struct priority_queue *);
struct list_elem *priority_queue_remove (struct priority_queue *, struct priority_queue_elem *);

/* PQ elements. */
struct priority_queue_elem *priority_queue_max (struct priority_queue *);

/* Miscellaneous. */
void priority_queue_verify (struct priority_queue *, bool is_sorted, list_less_func *, list_eq_func *);
bool priority_queue_empty (struct priority_queue *);
size_t priority_queue_size (struct priority_queue *);

#endif /* lib/kernel/priority_queue.h */
