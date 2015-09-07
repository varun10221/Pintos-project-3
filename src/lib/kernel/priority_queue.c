#include "priority_queue.h"
#include "../debug.h"

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

/* Add this element to the priority queue. */
void
priority_queue_push_back(struct priority_queue *pq, struct priority_queue_elem *elem)
{
  ASSERT (pq != NULL);
  ASSERT (elem != NULL);

  priority elem_p = *elem->p;
  ASSERT (PRI_MIN <= elem_p && elem_p <= PRI_MAX);

  pq->size++;  
  list_push_back (&pq->queue[elem_p], &elem->elem);
}

/* Insert into this priority queue. 
   We use the p value of the elem to determine the appropriate list,
   and then do a sorted insert into the list using list_less_func */
void
priority_queue_insert_ordered (struct priority_queue *pq, struct priority_queue_elem *elem, list_less_func *f, void *aux UNUSED)
{
  ASSERT (pq != NULL);
  ASSERT (elem != NULL);
  ASSERT (f != NULL);

  priority elem_p = *elem->p;
  ASSERT (PRI_MIN <= elem_p && elem_p <= PRI_MAX);

  pq->size++;  
  list_insert_ordered (&pq->queue[elem_p], &elem->elem, f, NULL);
}

/* Returns the highest priority element in the queue (round-robin in the event
   of a tie), or NULL if the queue is empty. */
struct priority_queue_elem *
priority_queue_pop_front(struct priority_queue *pq)
{
  ASSERT (pq != NULL);

  int i;
  struct priority_queue_elem *front = NULL;
  for (i = PRI_QUEUE_NLISTS-1; i >= 0; i--)
  {
    struct list *l = &pq->queue[i];
    if (!list_empty (l))
    {
      pq->size--;  
      front = list_entry (list_pop_front (l), struct priority_queue_elem, elem);
      /* Must be in the list matching its effective priority. */
      ASSERT (*front->p == i);
      break;
    }
  }
  return front;
}

/* Returns the next element in the priority queue list to which elem belongs.
   If there are no other elements at this priority level, returns the end of 
   the list. 
   
   Returning a list_elem instead of a priority_queue_elem allows you to iterate
   over each list in turn. */ 
struct list_elem *
priority_queue_remove (struct priority_queue *pq, struct priority_queue_elem *elem)
{
  ASSERT (pq != NULL);

  pq->size--;
  return list_remove (&elem->elem);
}

/* Identify the maximum element in the priority_queue. 
   Returns NULL if queue is empty. 
   Does not remove the element, just returns a pointer to it. */
struct priority_queue_elem *
priority_queue_max(struct priority_queue *pq)
{
  ASSERT (pq != NULL);

  int i;
  struct priority_queue_elem *max = NULL;

  for (i = PRI_QUEUE_NLISTS-1; i >= 0; i--)
  {
    struct list *l = &pq->queue[i];
    if (!list_empty (l))
    {
      max = list_entry (list_front (l), struct priority_queue_elem, elem);
      /* Must be in the list matching its effective priority. */
      ASSERT (*max->p == i);
      break;
    }
  }
  return max;
}

/* Verify that this priority queue is internally consistent. 
     1. Size is correct.
     2. Each element is in the correct list. 
     3. If is_sorted, verifies that each element is less than the following element.
        We don't know aux, so less and eq must be able to work with just a and b.
   For debugging only. */
void
priority_queue_verify (struct priority_queue *pq, bool is_sorted, list_less_func *less, list_eq_func *eq)
{
  ASSERT (pq != NULL);
  if (is_sorted)
  {
    ASSERT (less != NULL);
    ASSERT (eq != NULL);
  }

  struct priority_queue_elem *pq_e = NULL;
  struct list *l = NULL;
  struct list_elem *l_cur = NULL;
  struct list_elem *l_prev = NULL;

  size_t n_elts = 0;

  int i;
  for (i = PRI_QUEUE_NLISTS-1; i >= 0; i--)
  {
    l = &pq->queue[i];
    l_prev = NULL;
    for (l_cur = list_begin (l); l_cur != list_end (l);
         l_cur = list_next (l_cur))
      {
        /* If sorted, prev <= cur */
        if(is_sorted && l_cur != NULL && l_prev != NULL)
        {
          ASSERT ( less(l_prev, l_cur, NULL) || eq(l_prev, l_cur, NULL) );
        }

        n_elts++;
        pq_e = list_entry (l_cur, struct priority_queue_elem, elem);
        ASSERT (*pq_e->p == i);
        l_prev = l_cur;
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


