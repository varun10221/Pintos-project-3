#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"
#include "filesys/file.h"
#include "vm/page.h"

#include <stdint.h>

/* Forward declarations. */
struct mmap_info;
struct page;

/* Parent maintains a list of children in its 'struct thread'.
   Child has a pointer to its entry in the parent's list.
   Parent allocates each child_process_info on the heap so that they are
   safe for both parent and child to access even if the other has
   exited.

   The parent MUST have called process_wait_for_child_load before 
     it returns from syscall_exec. Otherwise we risk a null pointer
     dereference in start_process.

   The embedded sema and lock allow parent and child to synchronize. */
struct child_process_info
{
  tid_t child_tid; /* For locating a child by tid in parent's child_list. Set by child at startup. */
  tid_t parent_tid; /* For debugging. Set by parent at initialization. */

  /* Operations on ref_count are made atomic using the associated lock. */
  struct lock ref_count_lock;
  int8_t ref_count; /* Number of threads with a reference to this struct. */

  /* status_sema is Up'd twice by the child and
     Down'd twice by the parent:
     Once each for did_child_load_successfully, child_exit_status.
     Parent must Down for did_...success first and child_exit_status second. */
  struct semaphore status_sema;
  bool did_child_load_successfully; /* True if load() works in start_process, false else. */
  int child_exit_status; /* Set when child calls syscall_exit. */

  /* For inclusion in the parent's child_list. */
  struct list_elem elem;
};

/* These are the types of fds a process can have open. */
enum fd_type
{
  FD_FILE, /* This fd is for a file. */
  FD_DIRECTORY /* This fd is for a directory. */
};


tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

/* Used by syscall.c */
void process_set_exit_status (int exit_status);

/* Functions for files. */
int process_new_file (const char *file);
int process_new_dir (const char *dir);
void * process_fd_lookup (int fd, enum fd_type);
int process_fd_inumber (int fd);
void process_fd_delete (int fd);
void process_close_all_fds (void);

/* Virtual memory interaction. */
void * process_observe_stack_address (void *);
void * process_get_min_observed_stack_address (void);
void process_page_table_init (void);
void process_page_table_destroy (void);
struct page * process_page_table_find_page (const void *);
void process_pin_page (struct page *);
void process_unpin_page (struct page *);
struct mmap_info * process_add_mapping (struct mmap_details *, void *, int);
void process_delete_mapping (struct mmap_info *mmap_info);

/* mmap support (part of virtual memory). */
mapid_t process_mmap_add (struct mmap_info *);
void process_mmap_remove (mapid_t);
struct mmap_info * process_mmap_lookup (mapid_t);
void process_mmap_remove_all (void);

/* Syscall handling. */
void * process_scratch_page_get (void);
void process_scratch_page_free (void);

#endif /* userprog/process.h */
