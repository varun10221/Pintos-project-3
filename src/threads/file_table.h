#ifndef THREADS_FILE_TABLE_H
#define THREADS_FILE_TABLE_H

#include "filesys/file.h"
#include <stddef.h>

/* A file_table is a dynamic array used to track
   the set of files associated with a process.

   Threads use them to track their set of open files.
   Complexity estimates in the context of threads:
     Lookup is O(1): A file descriptor (fd) is an index into the file_arr.
     Insertion is O(N) on average: Adding a new file requires locating the first unused index,
       allocating a struct file, and populating it. See "Growth".
     Deletion is O(1): Deleting a file requires freeing the associated memory and
       setting the pointer to NULL. 
     Growth is O(N): If the file_table is full, we allocate a new one twice as large,
       copy the pointers over, and continue. Existing fds remain valid because the
       index into the table is unchanged. This puts the maximum number of
       files at SIZE_MAX. 
       
     Operations on this structure are NOT thread safe. 
     This is OK because we do not support user-level threads (i.e. sub-processes). */
struct file_table
{
  size_t n_elts;           /* Number of elements in the table. */
  size_t max_elts;         /* Max number of elements in the table. */
  struct file **file_arr;  /* Pointer to file pointers. Allows for cheap resizing. */
};

/* Creation and deletion. */
void file_table_init (struct file_table *, int init_size);
void file_table_destroy(struct file_table *);

/* Adding and deleting elements. */
int file_table_new_fd (struct file_table *, const char *name);
struct file * file_table_lookup (struct file_table *, int fd);
void file_table_delete_fd (struct file_table *, int fd);

#endif /* threads/file_table.h */
