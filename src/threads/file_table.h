#ifndef THREADS_FILE_TABLE_H
#define THREADS_FILE_TABLE_H

#include "filesys/file.h"
#include <stddef.h>

typedef int32_t ft_id_t;

/* A file_table is a dynamic array used to track
   the set of files associated with a process.

   Maps an integer id to a struct file*.

   Threads use them to track their set of open and mmap'd files.
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
int file_table_new_entry_by_name (struct file_table *, const char *name);
int file_table_new_entry_by_file (struct file_table *, struct file *f);
struct file * file_table_lookup (struct file_table *, ft_id_t id);
void file_table_delete_entry (struct file_table *, ft_id_t id);

#endif /* threads/file_table.h */
