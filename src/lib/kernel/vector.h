#ifndef __LIB_KERNEL_VECTOR_H
#define __LIB_KERNEL_VECTOR_H

#include <stdint.h>
#include <stddef.h>

/* A vector is an array that grows dynamically.
   Maps an id_t to a void * of arbitrary type.

   Lookup is O(1): An id_t is an index into the elts array.
   Insertion is O(N) on average: Adding a new entry requires locating the first 
     unused index.
   Deletion is O(1): Deleting a file requires freeing the associated memory and 
     setting the pointer to NULL. 
   Growth is O(N): If the file_table is full, we allocate a new one twice as large,
     copy the pointers over, and continue. Existing fds remain valid because the
     index into the table is unchanged. This puts the maximum number of
     files at SIZE_MAX. 
       
   Operations on this structure are NOT thread safe. */
struct vector
{
  size_t n_elts;       /* Number of elements in the table. */
  size_t max_elts;     /* Max number of elements in the table at its current size. */
  void **elts;  /* Pointer to arbitrary elements. Allows for cheap resizing. */
};

/* Creation and deletion. */
void vector_init (struct vector *, int init_size);
void vector_destroy (struct vector *);

/* Adding and deleting elements. */
id_t vector_add_elt (struct vector *, void *entry);
void * vector_lookup (struct vector *, id_t id);
void * vector_delete_elt (struct vector *, id_t id);

/* Performs some operation on each vector element elt (including NULL entries), given auxiliary data AUX. */
typedef void vector_action_func (void *elt, void *aux);
void vector_foreach (struct vector *, vector_action_func *, void *aux);

#endif /* lib/kernel/vector.h */
