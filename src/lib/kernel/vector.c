#include "vector.h"
#include "../debug.h"

#include <stdbool.h>
#include <string.h>
#include "threads/malloc.h"

/* Private functions. */
static bool vector_grow (struct vector *vec, int new_size);

/* Creation and deletion. */

/* Initialize a vector with space for this many elements. */
void 
vector_init (struct vector *vec, int init_size)
{
  ASSERT (vec != NULL);
  ASSERT (0 <= init_size);

  vec->n_elts = 0;
  vec->max_elts = init_size;

  if (vec->max_elts)
  {
    vec->elts = (void **) malloc (sizeof(void *) * vec->max_elts);
    ASSERT (vec->elts != NULL);
    /* Initialize each index to NULL. */
    size_t i;
    for (i = 0; i < vec->max_elts; i++)
      vec->elts[i] = NULL;
  }
  else
    vec->elts = NULL;
}

/* Destroy this vector. 
 
   The caller is responsible for freeing the memory associated with the 
   elements in the vector. */
void 
vector_destroy (struct vector *vec)
{
  ASSERT (vec != NULL);

  if (vec->elts != NULL)
  {
    free(vec->elts); 
    vec->elts = NULL;
  }

  vec->n_elts = 0;
  vec->max_elts = 0;
}

/* Adding and deleting elements. */

/* Add this entry in the first available slot. 
   Returns its ID, or -1 if we could not add the entry. */
id_t 
vector_add_elt (struct vector *vec, void *elt)
{
  ASSERT (vec != NULL);
  ASSERT (elt != NULL);

  size_t i, first_ix_to_check = 0, orig_max = vec->max_elts;

  /* Is there space for a new elt? */
  if (vec->n_elts == vec->max_elts)
  {
    /* Start the array at size 8; small enough to handle many typical interactions
       without needing to grow again. */
    bool grew = vector_grow (vec, (0 < vec->max_elts ? 2*vec->max_elts : 8));
    if (grew)
      /* vec was full, so the first empty elt will be the old max_elts. */
      first_ix_to_check = orig_max;
    else
      return -1;
  }

  for (i = first_ix_to_check; i < vec->max_elts; i++)
  {
    /* A spot! */
    if (vec->elts[i] == NULL)
    {
      vec->elts[i] = elt;
      vec->n_elts++;
      return i;
    }
  }

  /* Cannot get here, since we grew the table if it was too small. */
  NOT_REACHED ();
}

/* Return the element with id ID. 
   Returns NULL if ID is invalid or no such element. */
void * 
vector_lookup (struct vector *vec, id_t id)
{
  ASSERT (vec != NULL);
  if (id < 0)
    return NULL;
  if (vec->max_elts <= (size_t) id)
    return NULL;
  return vec->elts[id];
}

/* Free this ID for new entries. Return the element 
   associated with it. */
void * 
vector_delete_elt (struct vector *vec, id_t id)
{
  ASSERT (vec != NULL);
  if (id < 0)
    return NULL;
  if (vec->max_elts <= (size_t) id)
    return NULL;

  void *ret = vec->elts[id];

  vec->elts[id] = NULL;
  vec->n_elts--;

  return ret;
}

/* Grow this vector in size. 
   Returns true on success, false on failure. 
   On failure, ft is unchanged. */
static bool
vector_grow (struct vector *vec, int new_size)
{
  ASSERT (vec != NULL);
  ASSERT (vec->max_elts < (size_t) new_size);

  void **vec_new_elts = (void **) malloc (sizeof(void*) * new_size);
  /* On allocation failure, return without modifying FT. */
  if (vec_new_elts == NULL)
    return false;
  memset (vec_new_elts, 0, sizeof(void *) * new_size);
  /*TODO:should we call realloc ? */
  if (vec->elts != NULL)
  {
    /* Copy values over -- the pointers are still valid. 
       Old file descriptors we gave out (i.e. indices into this table) remain valid. */
    memcpy(vec_new_elts, vec->elts, sizeof(void *) * vec->max_elts);
    /* Free old elts, but don't free the files to which it points. */
    free(vec->elts); vec->elts = NULL;
  }

  /* Commit changes. */
  vec->max_elts = new_size;
  vec->elts = vec_new_elts;

  return true;
}

/* Apply f to each element of vec, including NULL entries. */
void vector_foreach (struct vector *vec, vector_action_func *func, void *aux)
{
  ASSERT (vec != NULL);
  ASSERT (func != NULL);

  size_t i;
  for (i = 0; i < vec->max_elts; i++)
    func (vec->elts[i], aux);
}
