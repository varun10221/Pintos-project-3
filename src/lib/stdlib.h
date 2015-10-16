#ifndef __LIB_STDLIB_H
#define __LIB_STDLIB_H

#include <stddef.h>

/* Some math routines. */

/* Various flavors of absolute value. */
static inline int abs (int j)
{
  return ( 0 < j ? j : -1*j);
}

static inline long int labs (long int j)
{
  return ( 0 < j ? j : -1*j);
}

static inline long long int llabs (long long int j)
{
  return ( 0 < j ? j : -1*j);
}

/* Return the min of X and Y. */
#define MIN(X, Y) \
        ((X) < (Y) ? (X) : (Y))

/* Return the max of X and Y. */
#define MAX(X, Y) \
        ((X) < (Y) ? (Y) : (X))

/* Standard functions. */
int atoi (const char *);
void qsort (void *array, size_t cnt, size_t size,
            int (*compare) (const void *, const void *));
void *bsearch (const void *key, const void *array, size_t cnt,
               size_t size, int (*compare) (const void *, const void *));

/* Nonstandard functions. */
void sort (void *array, size_t cnt, size_t size,
           int (*compare) (const void *, const void *, void *aux),
           void *aux);
void *binary_search (const void *key, const void *array, size_t cnt,
                     size_t size,
                     int (*compare) (const void *, const void *, void *aux),
                     void *aux);

#endif /* lib/stdlib.h */
