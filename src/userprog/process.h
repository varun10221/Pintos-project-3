#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"
#include "filesys/file.h"

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

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

/* Used by syscall.c */
void process_set_exit_status (int exit_status);

#endif /* userprog/process.h */
