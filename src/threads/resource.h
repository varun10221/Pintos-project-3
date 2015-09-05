#ifndef THREADS_RESOURCE_H
#define THREADS_RESOURCE_H

/* Forward declarations to avoid compiler errors. */
struct thread;
struct list_elem;

/* Generic resource used to provide polymorphism.
   As long as a resource's first two elements are a struct thread* indicating
   the holder of the resource, and a list element, we can implement 
   priority and nested donation without knowing the type of resource. */
struct resource
  {
    struct thread *holder; /* Who owns this resource? */
    struct list_elem elem; /* For including resource in lists. */
    struct priority_queue *waiters;  /* Prioritized list of waiting threads. */
  };

#endif /* threads/resource.h */
