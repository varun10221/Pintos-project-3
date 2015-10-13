#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/stack.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "vm/page.h"

/* Structure for command-line args. */
struct cl_args
{
  int argc;   /* Number of args. */
  int total_arglen; /* Sum of lengths of arg strings. */
  char *args; /* Sequence of c-strings, one per arg. */
};

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
bool load_args_onto_stack (void **esp, struct cl_args *args);

/* IPC between parent and child for syscall_{exec,wait,exit}. */

/* Used by parent. */
static struct child_process_info * process_parent_prepare_child_info (void);
static struct child_process_info * process_parent_lookup_child (tid_t child);

/* Used by child. */
static void process_set_load_success (bool did_load_succeed);
static void process_signal_loaded (void);
/* Used by parent. */
static bool process_wait_for_child_load (struct child_process_info *cpi);

/* Used by child. */
static void process_signal_exiting (void);
/* Used by parent. */
static int process_wait_for_child_exit (tid_t child);
void process_parent_discard_children (void);

/* Used by parent and child. */
static void child_process_info_atomic_inc_refcount (struct child_process_info *cpi);
static bool child_process_info_atomic_dec_refcount (struct child_process_info *cpi);

/* Process sets whether or not it loaded successfully. */
static void
process_set_load_success (bool did_load_succeed)
{
  thread_get_child_info_self ()->did_child_load_successfully = did_load_succeed; 
}

/* Child signals that it has set its load status. */
static void 
process_signal_loaded (void)
{
  sema_up (&thread_get_child_info_self ()->status_sema);
}

/* Child sets its exit status. */
void
process_set_exit_status (int exit_status)
{
  thread_get_child_info_self ()->child_exit_status = exit_status;
}

/* Child signals that it has exited. 
   Child must not touch its child_info_self pointer again. */
static void 
process_signal_exiting (void)
{
  struct child_process_info *cpi = thread_get_child_info_self ();
  /* If a kernel thread, simply return. */
  if (cpi == NULL) return;

  sema_up (&cpi->status_sema);
  child_process_info_atomic_dec_refcount (cpi);
}

/* Wait until child loads, then return its load status.
   True if successfully loaded, else false. */
static bool
process_wait_for_child_load (struct child_process_info *cpi)
{
  ASSERT (cpi != NULL);

  sema_down (&cpi->status_sema);
  return cpi->did_child_load_successfully;
}

/* Wait until child exits, then return its exit status. */
static int
process_wait_for_child_exit (tid_t child)
{
  /* Invalid tid. */
  if(child < 0)
    return -1;

  struct child_process_info *cpi = process_parent_lookup_child (child);
  /* No such child: invalid or we waited already. */
  if (cpi == NULL)
    return -1;

  /* First time we are waiting on a valid child. */ 
  list_remove (&cpi->elem);

  /* Wait for child to mark itself done. */
  sema_down (&cpi->status_sema);

  /* Get status and decrement the refcount. */
  int status = cpi->child_exit_status;
  child_process_info_atomic_dec_refcount (cpi);
  return status;
}

/* For use when a process is exiting.
   Parent decrements the ref count associated with each child
   on which it never wait'd. */
void process_parent_discard_children (void)
{
  struct list *child_list = &thread_current ()->child_list;
  struct child_process_info *cpi;
  struct list_elem *e;
  while (!list_empty (child_list))
  {
    e = list_pop_front (child_list);
    cpi = list_entry (e, struct child_process_info, elem);
    child_process_info_atomic_dec_refcount (cpi);
  }
}

/* Return the cpi associated with this CHILD.
   Returns NULL if we have no record of this CHILD.
    'No record' could occur for two reasons:
      1. We did not make a CHILD with this tid_t.
      2. We made such a CHILD but have already called process_wait_for_child_exit. 

   CHILD is expected to be at least 0. */
static struct child_process_info * 
process_parent_lookup_child (tid_t child)
{
  ASSERT (0 <= child);

  struct list_elem *e;

  struct list *child_list = &thread_current ()->child_list;
  for (e = list_begin (child_list); e != list_end (child_list);
       e = list_next (e))
    {
      struct child_process_info *cpi = list_entry (e, struct child_process_info, elem);
      if (cpi->child_tid == child)
        return cpi;
    }
  return NULL;
}

/* Prepare a new 'struct child_process_info' with refcount 1.
   Add it to the parent's child_list. */
static struct child_process_info * 
process_parent_prepare_child_info (void)
{
  struct child_process_info *cpi = (struct child_process_info *) malloc(sizeof(struct child_process_info));
  if (cpi == NULL)
    return NULL;

  cpi->child_tid = -1;
  cpi->parent_tid = thread_tid ();
  lock_init (&cpi->ref_count_lock);
  sema_init (&cpi->status_sema, 0);
  cpi->did_child_load_successfully = false;
  /* Unless child explicitly sets his exit status, we exit in failure.
     e.g. being killed due to a page fault. */
  cpi->child_exit_status = TID_ERROR;

  /* Add to parent's child_list. */
  list_push_back (&thread_current ()->child_list, &cpi->elem);
  /* Parent has a reference to this cpi. */
  cpi->ref_count = 1;

  return cpi;
}

/* Atomically increment the ref count. */
static void 
child_process_info_atomic_inc_refcount (struct child_process_info *cpi)
{
  ASSERT (cpi != NULL);

  lock_acquire (&cpi->ref_count_lock);
  cpi->ref_count++;
  lock_release (&cpi->ref_count_lock);
}

/* Atomically decrement the ref count. If it is 0, we free the cpi.
   Returns true if the cpi is still valid, false else. */
static bool 
child_process_info_atomic_dec_refcount (struct child_process_info *cpi)
{
  ASSERT (cpi != NULL);

  bool is_still_valid = true;

  lock_acquire (&cpi->ref_count_lock);
  cpi->ref_count--;

  /* Were we the last to hold a reference? 
   
     NB: do this test atomically to avoid race
      with parent and child decrementing concurrently and
      then both trying to free the cpi. */
  if (cpi->ref_count == 0)
  {
    lock_release (&cpi->ref_count_lock);
    free (cpi);
    is_still_valid = false;
  }
  else{
    lock_release (&cpi->ref_count_lock);
  }

  return is_still_valid;
}

/* Starts a new thread running a user program loaded from
   ARGS. The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. 

   Will not return until the new thread has loaded
   its executable and signald whether it succeeded or failed.
  
   ARGS: file_name [arg1 arg2 ...] */
tid_t
process_execute (const char *args) 
{
  char *args_copy;
  struct cl_args *cl_args;
  tid_t tid;

  /* Arg checking. 
     Failure == kernel bug: syscall_exec should not have given us a NULL pointer. */
  ASSERT (args != NULL);

  /* Make sure the args (formatted for start_process) will not be 
     too long to fit into PGSIZE. */
  if (PGSIZE < sizeof(void *) + sizeof(struct cl_args) + strlen (args))
    return TID_ERROR; 

  /* Make a copy of ARGS so that we can tokenize it. */
  args_copy = palloc_get_page (0);
  if (args_copy == NULL)
    return TID_ERROR;
  strlcpy (args_copy, args, PGSIZE);

  void *thr_args = palloc_get_page (0);
  if (thr_args == NULL)
    return TID_ERROR;

  /* Prepare a child info object and put a reference to it at the beginning of thr_args. */
  struct child_process_info *cpi = process_parent_prepare_child_info ();
  if (cpi == NULL)
    return TID_ERROR;
  memcpy (thr_args, &cpi, sizeof(void *));

  /* Prepare a cl_args to pass to start_process. */
  cl_args = (struct cl_args *) (thr_args + sizeof(void *));

  cl_args->argc = 0;
  cl_args->total_arglen = 0;
  /* Args is in a fresh page, so point it to just past itself. */
  cl_args->args = (char *) cl_args + offsetof(struct cl_args, args) + sizeof(char *);

  /* Iterate over arguments, tokenized by whitespace. */
  char *token, *args_copy_save_ptr, *cl_args_ptr;
  cl_args_ptr = cl_args->args;
  size_t len;
  for (token = strtok_r (args_copy, " ", &args_copy_save_ptr); token != NULL;
       token = strtok_r (NULL, " ", &args_copy_save_ptr))
  {
    cl_args->argc++;
    len = strlcpy(cl_args_ptr, token, PGSIZE);
    /* Skip ahead over the string we just wrote, including null byte. */
    cl_args_ptr += len + 1;
    /* Count this length, including the null byte. */
    cl_args->total_arglen += len + 1;
  }

  /* Free our working memory. */
  palloc_free_page (args_copy);

  /* No arguments means there is no file name to execute (e.g. if user provided an empty string). */
  if (cl_args->argc <= 0)
    return TID_ERROR;

  /* Create a new thread to execute FILE_NAME. */

  /* First string is the file_name. */
  const char *file_name = cl_args->args;
  tid = thread_create (file_name, PRI_DEFAULT, start_process, thr_args);

  /* start_process will free thr_args if it starts successfully.
     If not, we have to clean this up ourselves. */
  if (tid == TID_ERROR)
    palloc_free_page (thr_args); 
  else
  {
    bool did_load_succeed = process_wait_for_child_load (cpi);
    if (!did_load_succeed)
    {
      /* Call wait so that we clean up the child_process_info struct. */
      process_wait_for_child_exit (cpi->child_tid);
      tid = TID_ERROR;
    }
  }
  
  return tid;
}

/* To set up the initial stack, track offsets
   of args using a list of int64_elem's. */
struct int64_elem
{
  int64_t val;
  struct list_elem elem;
};

/* A thread function that loads a user process and starts it
   running with its args. Does not return. 
   
   THR_ARGS format:
     child_process_info* (pointer to a shared cpi)
     struct cl_args
   */
static void
start_process (void *thr_args)
{
  ASSERT (thr_args != NULL);

  /* Extract the child info reference and then process the cl_args. 
     Race condition here on cpi, so parent must call process_wait_for_child_load
     to ensure we've done this setup step. */
  struct child_process_info *cpi = (struct child_process_info *) *(struct child_process_info **) thr_args;

  cpi->child_tid = thread_tid ();
  thread_set_child_info_self (cpi);

  /* Do this before we signal the parent that we have loaded, lest the
     parent exit and free the cpi before we call exit(). */
  child_process_info_atomic_inc_refcount (cpi);

  struct cl_args *cl_args = (struct cl_args *) (thr_args + sizeof(void *));
  char *file_name = cl_args->args;
  struct intr_frame if_;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  /* Load executable. */ 

  /* Grab the filesys_lock, allowing us to file_deny_write and then load() atomically. */
  bool success = load (file_name, &if_.eip, &if_.esp);

  /* Set and signal load status. */
  process_set_load_success (success);
  process_signal_loaded ();

  /* If we've failed to deny writes to the executable and load it, quit. */
  if (!success) 
  {
    process_set_exit_status (-1);
    thread_exit ();
  }

  /* Load succeeded. esp is the stack pointer. Load up the arguments. */
  bool could_load_args = load_args_onto_stack (&if_.esp, cl_args);
  /* If we could not load args, quit. */
  if (!could_load_args)
  {
    process_set_exit_status (-1);
    thread_exit ();
  }

  /* Free the args, now that we're done with them . */
  palloc_free_page (thr_args);
  
  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Load args onto the stack specified by esp. 
   Returns true on success, false on failure. */
bool
load_args_onto_stack (void **esp, struct cl_args *cl_args)
{
  ASSERT (esp != NULL && *esp != NULL);
  ASSERT (cl_args != NULL);

  /* Make sure the quantity of args won't overflow the stack page. 
     We put more information into it than here we did in process_execute, so the
     sizeof test we did there is insufficient.

     esp can hold PGSIZE bytes, based on how it is initialized
     in setup_stack.

     How much data, total, will be put into the stack page? */
  int64_t space_needed = 
    cl_args->total_arglen + /* Total length of all arg strings. */
    4 + /* Round down to a multiple of 4, adding at most 4 bytes. */
    sizeof(void *) + /* Null pointer sentinel. */
    sizeof(void *)*cl_args->argc + /* Pointers to each of the args. */
    sizeof(void *)*2 + /* Location of argv, fake return address. */
    sizeof(int); /* argc. */

  if (PGSIZE < space_needed)
  {
    /* If a page cannot hold all of this, return failure. */
    return false;
  }

  /* Calculate the starting offset of each string. */
  struct int64_elem *ie = NULL; 
  struct list offset_list;
  list_init (&offset_list);

  /* Offset into cl_args->args. */
  int64_t off = 0;
  int i;
  for (i = 0; i < cl_args->argc; i++)
  {
    ie = (struct int64_elem *) malloc(sizeof(struct int64_elem));
    ie->val = off;
    list_push_back (&offset_list, &ie->elem);
    /* Skip to the next string. */
    off += strlen (cl_args->args + off) + 1;
  }

  char *orig_stack_top = (char *) *esp;

  /* Push arguments onto stack in reverse order. */
  char *curr = NULL;
  struct list_elem *e = NULL;
  for (e = list_rbegin (&offset_list); e != list_rend (&offset_list);
       e = list_prev (e))
  {
    ie = list_entry (e, struct int64_elem, elem);
    /* Calculate offset of the next arg to push. */
    curr = cl_args->args + ie->val;
    stack_push_string (esp, curr);
  }
  /* Round down to multiple of 4. */
  stack_align (esp, 4);
  /* Push a null pointer sentinel. */
  stack_push_ptr (esp, NULL);

  /* Push the location of each of the args I pushed above. */
  char *curr_sp = orig_stack_top;
  for (e = list_rbegin (&offset_list); e != list_rend (&offset_list);
       e = list_prev (e))
  {
    ie = list_entry (e, struct int64_elem, elem);
    /* Calculate offset of this arg. */
    curr = cl_args->args + ie->val;
    curr_sp -= strlen (curr) + 1;
    stack_push_ptr (esp, (void *) curr_sp);
  }

  /* Push the location of argv itself, which is located at
     the current location of the stack pointer. */
  curr_sp = (char *) *esp;
  stack_push_ptr (esp, curr_sp);

  /* Push argc. */
  stack_push_int (esp, cl_args->argc);
  /* Push fake return address. */
  stack_push_ptr (esp, NULL);

  /* Clean up memory. */
  while (!list_empty (&offset_list))
  {
    struct list_elem *e = list_pop_front (&offset_list);
    ie = list_entry (e, struct int64_elem, elem);
    free(ie);
  }

  return true;
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting. */
int
process_wait (tid_t child) 
{
  return process_wait_for_child_exit (child);
}

/* Free the current process's resources. 
   All exiting processes must come through here to avoid leaks. 
   Called from thread_exit. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  process_close_all_files ();
  /* Not required, but keep user processes out of trouble if they happen to share locks. */
  thread_release_all_locks ();

  /* Indicate that we no longer have a reference to any un-waited children. 
     If they exited already, we clean up the shared CPI. If not, they will
     clean it up when they exit. */
  process_parent_discard_children ();

  /* Unlock executable, if we opened one. */
  if (cur->my_executable != NULL)
    file_close (cur->my_executable);

  /* Announce that we're exiting. Do so BEFORE we potentially free our child_info_self. 
     Only announce if we were a valid thread. */
  struct child_process_info *cpi = thread_get_child_info_self ();
  if(cpi->did_child_load_successfully) 
    printf("%s: exit(%d)\n", thread_name (), cpi->child_exit_status);

  /* Tell our parent that we're exiting.
     Exit status must have been set already (via process_set_exit_status()). 
     Default exit status is -1. */
  process_signal_exiting ();

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  filesys_lock ();

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  t->my_executable = filesys_open (file_name);
  if (t->my_executable == NULL)
    goto done;

  /* Convenient shorthand... */
  file = t->my_executable;
  /* 3.3.5: Lock writes to the executable while we are using it. 
     Closing a file will re-enable writes. 
     t->my_executable is closed in process_exit. */
  file_deny_write (file);
  /* File is now read-only: safe to load executable. */

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  filesys_unlock ();
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

/* Functions for files. */

/* Convert fd to index in a thread's fd_table. */
static id_t
fd_to_ix (int fd)
{
  ASSERT (N_RESERVED_FILENOS <= fd);
  return fd - N_RESERVED_FILENOS;
}

/* Convert index in a thread's fd_table to an fd. */
static int
ix_to_fd (id_t ix)
{
  ASSERT (0 <= ix);
  return ix + N_RESERVED_FILENOS;
}

/* Open a new fd corresponding to this file. 
   Caller must hold filesys lock. 

   Returns the fd, or -1 on failure. */
int
process_new_file (const char *file)
{
  ASSERT (file != NULL);

  struct file *f = filesys_open (file);
  if (f == NULL)
    return -1;
  return ix_to_fd (vector_add_elt (&thread_current ()->fd_table, f));
}

/* Return the 'struct file*' associated with this fd. 

   Returns NULL if there is no such fd. */ 
struct file * 
process_fd_lookup (int fd)
{
  if (fd < N_RESERVED_FILENOS)
    return NULL;
  return (struct file *) vector_lookup (&thread_current ()->fd_table, fd_to_ix (fd));
}

/* Close the file instance associated with this fd. 
   Caller must hold filesys lock. 
   If there is no such fd, does nothing. */
void 
process_fd_delete (int fd)
{
  if (fd < N_RESERVED_FILENOS)
    return;
  struct file *f = (struct file *) vector_delete_elt (&thread_current ()->fd_table, fd_to_ix (fd) );
  if (f != NULL)
    file_close (f);
}

/* Close this file. For use with vector_foreach. 
   Acquires and releases filesys_lock. */
static void
process_close_file (void *elt, void *aux UNUSED)
{
  struct file *f = (struct file *) elt;
  if (f == NULL)
    return;

  filesys_lock ();
  file_close (f);
  filesys_unlock ();
}

/* Close all open file handles and free the memory.
   Use when a process is exiting. */
void 
process_close_all_files (void)
{
  struct vector *vec = &thread_current ()->fd_table;
  /* Each call acquires and releases filesys_lock. */
  vector_foreach (vec, process_close_file, NULL);
  vector_destroy (vec);
}

/* mmap support. */

/* Add an entry for this mmap_info in the process's mmap_table. 
 
   Returns mapid, or -1 on failure. */ 
mapid_t process_mmap_add (struct mmap_info *mmap_info)
{
  ASSERT (mmap_info != NULL);

  return vector_add_elt (&thread_current ()->mmap_table, (void *) mmap_info);
}

/* Remove this mapping. 
   Caller is responsible for freeing the memory associated with it. */
void process_mmap_remove (mapid_t id)
{
  ASSERT (0 <= id); 
  vector_delete_elt (&thread_current ()->mmap_table, id);
}

/* Find the struct file* associated with this id. 
   Returns NULL if no mapping. */
struct mmap_info * process_mmap_lookup (mapid_t id)
{
  ASSERT (0 <= id); 

  return (struct mmap_info *) vector_lookup (&thread_current ()->mmap_table, id);
}

/* Destroy this mapping. For use with vector_foreach. */
static void
process_mmap_destroy_mapping (void *elt, void *aux UNUSED)
{
  struct mmap_info *mmap_info = (struct mmap_info *) elt;
  if (mmap_info == NULL)
    return;

  ASSERT (mmap_info->seg != NULL);
  process_delete_mapping (mmap_info);
}

/* Remove all extant mappings from the mmap_table and free the memory.
   Use when a process is exiting. */
void process_mmap_remove_all (void)
{
  struct vector *vec = &thread_current ()->mmap_table;
  vector_foreach (vec, process_mmap_destroy_mapping, NULL);
  vector_destroy (vec);
}

/* Page table interaction functions. */

/* Return the appropriate page from supplemental page table,
     or NULL if no such page is defined. */
struct page * process_page_table_find_page (void *vaddr)
{
  return supp_page_table_find_page (&thread_current ()->supp_page_table, vaddr); 
}

/* Add a memory mapping to this process's page table 
     for file F beginning at START with flags FLAGS.
   Returns NULL on failure.

   F must be a "dup"; process will close it when the mapping 
     is destroyed. 

   Caller should NOT free the mmap_info.
   Use process_delete_mapping() to clean up the mmap_info. */
struct mmap_info * process_add_mapping (struct file *f, void *start, int flags)
{
  ASSERT (f != NULL);
  ASSERT (start != NULL);

  struct segment *seg = NULL;
  struct mmap_info *mmap_info = NULL;

  seg = supp_page_table_add_mapping (&thread_current ()->supp_page_table, f, start, flags, false);
  if (seg == NULL)
    goto CLEANUP_AND_ERROR;

  mmap_info = (struct mmap_info *) malloc (sizeof(struct mmap_info));
  if (mmap_info == NULL)
    goto CLEANUP_AND_ERROR;

  mmap_info->seg = seg;

  return mmap_info;

  CLEANUP_AND_ERROR:
    if (seg != NULL)
      supp_page_table_remove_segment (&thread_current ()->supp_page_table, seg);
    if (mmap_info != NULL)
      free (mmap_info);
    return NULL;
}

/* Remove the memory mapping specified by MMAP_INFO. */
void process_delete_mapping (struct mmap_info *mmap_info)
{
  ASSERT (mmap_info != NULL);
  ASSERT (mmap_info->seg != NULL);

  supp_page_table_remove_segment (&thread_current ()->supp_page_table, mmap_info->seg);
  free (mmap_info);
}
