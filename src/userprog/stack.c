#include "userprog/stack.h"
#include "threads/malloc.h"
#include <string.h>

/* Push this string (including terminating null byte) onto stack. */
void *
stack_push_string (void **sp, char *str)
{
  ASSERT (sp != NULL);
  ASSERT (*sp != NULL);
  ASSERT (str != NULL);

  return stack_push_raw (sp, str, strlen(str) + 1);
}

/* Push this int onto stack. */
void *
stack_push_int (void **sp, int i)
{
  ASSERT (sp != NULL);
  ASSERT (*sp != NULL);
  return stack_push_raw (sp, &i, sizeof(int));
}

/* Push this int32 onto stack. */
void *
stack_push_int32 (void **sp, int32_t i)
{
  ASSERT (sp != NULL);
  ASSERT (*sp != NULL);

  return stack_push_raw (sp, &i, sizeof(int32_t));
}

/* Push this address onto stack. */
void *
stack_push_ptr (void **sp, void *ptr)
{
  ASSERT (sp != NULL);
  ASSERT (*sp != NULL);
  
  if(ptr)
    return stack_push_raw (sp, &ptr, sizeof(void *));
  else
  {
    /* Null pointer is 000...0 */
    int64_t zero = 0;
    return stack_push_raw (sp, &zero, sizeof(void *));
  }
  NOT_REACHED ();
}

/* Round stack down to nearest multiple of align. */
void *
stack_align (void **sp, size_t align)
{
  ASSERT (0 < align);
  uint32_t diff = (uint32_t) *sp % align;
  /* Shift down the appropriate amount and zero out the memory. */
  if (diff)
  {
    *sp -= diff;
    memset(*sp, 0, diff);
  }
  return *sp;
}

/* Push this data onto the stack, moving sp down first.
   Returns the resulting location of sp. */
void *
stack_push_raw (void **sp, void *data, size_t n_bytes)
{
  ASSERT (sp != NULL);
  ASSERT (*sp != NULL);
  ASSERT (data != NULL);
  ASSERT (0 < n_bytes); 

  /* Make sure sp won't go below zero when we subtract n_bytes. */
  ASSERT ((void *) n_bytes <= *sp);

  /* Decrement, then push. */
  *sp -= n_bytes;
  memcpy (*sp, data, n_bytes);
  return *sp;
}

/* Returns pointer to the current value on the stack,
   and increments the stack by one pointer. */ 
void *
stack_pop (void **sp)
{
  ASSERT (sp != NULL);
  ASSERT (*sp != NULL);

  /* Grab value, then increment. */
  void *curr = *sp;
  *sp += sizeof(void *);
  return curr;
}
