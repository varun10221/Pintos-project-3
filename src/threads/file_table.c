#include "threads/file_table.h"

#include <stdint.h>
#include <debug.h>
#include <stdio.h>
#include "threads/malloc.h"
#include <string.h>
#include "filesys/filesys.h"

static bool file_table_grow (struct file_table *, int new_size);
static int file_table_ix_to_fd (int ix);
static int file_table_fd_to_ix (int fd);

/* Initialize a file table with space for this many elements. */
void 
file_table_init (struct file_table *ft, int init_size)
{
  ASSERT (ft != NULL);
  ASSERT (0 <= init_size);
  size_t i;

  ft->n_elts = 0;
  ft->max_elts = init_size;

  if (ft->max_elts)
  {
    ft->file_arr = (struct file **) malloc (sizeof(struct file*) * ft->max_elts);
    ASSERT (ft->file_arr != NULL);
    /* Initialize each index to NULL. */
    for (i = 0; i < ft->max_elts; i++)
      ft->file_arr[i] = NULL;
  }
  else
    ft->file_arr = NULL;
}

/* Destroy this file table, closing all open files. */
void 
file_table_destroy (struct file_table *ft)
{
  ASSERT (ft != NULL);

  /* Close and free all active files. */
  int init_n_elts = ft->n_elts;
  int n_found = 0;

  size_t i;
  if (ft->file_arr != NULL)
  {
    for (i = 0; i < ft->max_elts; i++)
    {
      if (ft->file_arr[i] != NULL)
      {
        /* This closes and frees. */
        file_close (ft->file_arr[i]);
        ft->file_arr[i] = NULL;
        ft->n_elts--;
        n_found++;
      }
    }
    free(ft->file_arr); ft->file_arr = NULL;
  }
  ASSERT (n_found == init_n_elts);

  ft->n_elts = 0;
  ft->max_elts = 0;
}

/* Add a new fd corresponding to FILE to FT.
   Will grow the file table if it is full. 
   Returns -1 if failure is encountered. */ 
int 
file_table_new_fd (struct file_table *ft, const char *name)
{
  ASSERT (ft != NULL);
  ASSERT (name != NULL);

  size_t i, first_ix_to_check = 0, orig_max = ft->max_elts;

  /* Is there space for a new file? */
  if (ft->n_elts == ft->max_elts)
  {
    /* Start the array at size 8; small enough to handle many typical process flows
       without needing to grow again? */
    bool grew = file_table_grow (ft, (0 < ft->max_elts ? 2*ft->max_elts : 8));
    if (grew)
      /* ft was full, so the first empty entry will be the old max_elts. */
      first_ix_to_check = orig_max;
    else
      return -1;
  }

  for (i = first_ix_to_check; i < ft->max_elts; i++)
  {
    if (ft->file_arr[i] == NULL)
    {
      ft->file_arr[i] = filesys_open (name);
      if (ft->file_arr[i] != NULL)
      {
        /* Successfully added an open file. */
        ft->n_elts++;
        return file_table_ix_to_fd (i);
      }
      else
        return -1;
    }
  }

  NOT_REACHED ();
}

/* Return the struct file * associated with this fd,
   or NULL if the fd is not associated with an open file. 
   Assumes N_RESERVED_FILENOS <= fd. */
struct file * 
file_table_lookup (struct file_table *ft, int fd)
{
  ASSERT (ft != NULL);

  if (fd < N_RESERVED_FILENOS)
    return NULL;

  int ix = file_table_fd_to_ix (fd);
  if (ft->max_elts < (size_t) ix)
    /* ix is beyond safety. */
    return NULL;
  return ft->file_arr[ix]; 
}

/* Delete this fd from the table. If no such
   fd is present, nothing happens. 
   If this fd is present, closes the file. */
void 
file_table_delete_fd (struct file_table *ft, int fd)
{
  ASSERT (ft != NULL);

  if (fd < N_RESERVED_FILENOS)
    return;

  int ix = file_table_fd_to_ix (fd);
  if (ft->max_elts <= (size_t) ix)
    /* ix is beyond safety. */
    return; 

  /* Close the file if it is non-null. */
  if(ft->file_arr[ix] != NULL)
  {
    file_close (ft->file_arr[ix]);
    ft->file_arr[ix] = NULL;
    ft->n_elts--;
  }
  return;
}

/* Grow this ft in size. 
   Returns true on success, false on failure. 
   On failure, ft is unchanged. */
static bool
file_table_grow (struct file_table *ft, int new_size)
{
  ASSERT (ft != NULL);
  ASSERT (ft->max_elts < (size_t) new_size);

  struct file **ft_new_file_arr = (struct file **) malloc (sizeof(struct file*) * new_size);
  /* On allocation failure, return without modifying FT. */
  if (ft_new_file_arr == NULL)
    return false;
  memset (ft_new_file_arr, 0, sizeof(struct file*) * new_size);

  if (ft->file_arr != NULL)
  {
    /* Copy values over -- the pointers are still valid. 
       Old file descriptors we gave out (i.e. indices into this table) remain valid. */
    memcpy(ft_new_file_arr, ft->file_arr, sizeof(struct file*) * ft->max_elts);
    /* Free old file_arr, but don't free the files to which it points. */
    free(ft->file_arr); ft->file_arr = NULL;
  }

  /* Commit changes. */
  ft->max_elts = new_size;
  ft->file_arr = ft_new_file_arr;

  return true;
}

/* Convert fd to index in file table. */
static int
file_table_fd_to_ix (int fd)
{
  ASSERT (N_RESERVED_FILENOS <= fd);
  return fd - N_RESERVED_FILENOS;
}

/* Convert index in file table to fd. */
static int
file_table_ix_to_fd (int ix)
{
  ASSERT (0 <= ix);
  return ix + N_RESERVED_FILENOS;
}
