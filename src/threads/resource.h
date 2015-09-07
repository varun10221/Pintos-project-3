#ifndef THREADS_RESOURCE_H
#define THREADS_RESOURCE_H

#include <list.h>
#include <priority_queue.h>

/* Generic resource used to provide polymorphism.
   As long as a resource's first two elements are a struct thread* indicating
   the holder of the resource, and a list element, we can implement 
   priority and nested donation without knowing the type of resource. */
struct resource
  {
    struct thread *holder; /* Who owns this resource? */
    /* TODO union? */
    struct priority_queue_elem pq_elem; /* For including resource in priority queues. */
    struct list_elem l_elem; /* For including resource in lists. */
    struct priority_queue *waiters;  /* Prioritized list of waiting threads. */
  };

#endif /* threads/resource.h */
