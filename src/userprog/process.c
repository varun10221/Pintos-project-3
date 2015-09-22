#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#include "threads/file_table.h"

/* Structure for command-line args. */
struct cl_args
{
  int argc;   /* Number of args. */
  char *args; /* Sequence of c-strings: null-delimited args. */
};

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

/* IPC between parent and child. */
struct child_process_info * process_parent_prepare_child_info (void);
struct child_process_info * process_parent_lookup_child (tid_t child);

void process_mark_loaded (bool did_load_successfully);
bool process_wait_for_child_load (tid_t child);

void process_mark_exiting (int exit_status);
int process_wait_for_child_exit (tid_t child);

void child_process_info_atomic_inc_refcount (struct child_process_info *cpi);
bool child_process_info_atomic_dec_refcount (struct child_process_info *cpi);

/* Child marks that it is loaded and sets its load status. */
void 
process_mark_loaded (bool did_load_successfully)
{
}

/* Child marks that it is exiting and sets its exit status. */
void 
process_mark_exiting (int exit_status)
{
}

/* Wait until child loads, then return its load status.
   True if successfully loaded, else false. */
bool
process_wait_for_child_load (tid_t child)
{
  ASSERT (0 <= child);
  return false;
}

/* Wait until child exits, then return its exit status. */
int
process_wait_for_child_exit (tid_t child)
{
  ASSERT (0 <= child);
  return -1;
}

/* Return the cpi associated with this child.
   Returns NULL if we have no record of this child.
    'No record' could occur for two reasons:
      1. We did not make a child with this tid_t.
      2. We made such a child but have already called process_wait_for_child_exit. */
struct child_process_info * 
process_parent_lookup_child (tid_t child)
{
  ASSERT (0 <= child);
  return NULL;
}

struct child_process_info * 
process_parent_prepare_child_info (void)
{
  return NULL;
}

/* Atomically increment the ref count. */
void child_process_info_atomic_inc_refcount (struct child_process_info *cpi)
{
  ASSERT (cpi != NULL);

  lock_acquire (&cpi->ref_count_lock);
  cpi->ref_count++;
  lock_release (&cpi->ref_count_lock);
}

/* Atomically decrement the ref count. If it is 0, we free the cpi.
   Returns true if the cpi is still valid, false else. */
bool child_process_info_atomic_dec_refcount (struct child_process_info *cpi)
{
  ASSERT (cpi != NULL);

  bool did_free = false;

  lock_acquire (&cpi->ref_count_lock);

  cpi->ref_count--;
  if (cpi->ref_count)
    lock_release (&cpi->ref_count_lock);
  else
  {
    free (cpi);
    did_free = true;
  }

  return did_free;
}

/* Starts a new thread running a user program loaded from
   ARGS. The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. 
  
   ARGS: file_name [arg1 arg2 ...]
   */
tid_t
process_execute (const char *args) 
{
  char *args_copy;
  struct cl_args *cl_args;
  tid_t tid;

  /* Arg checking. */
  ASSERT (args != NULL);
  /* Make sure the args (formatted as a struct cl_args) will not be 
     too long to fit into PGSIZE. 
     
     TODO This should be handled more cleanly than an ASSERT. */
  ASSERT (strlen (args) + sizeof(struct cl_args) < PGSIZE);

  /* Make a copy of ARGS so that we can tokenize it. */
  args_copy = palloc_get_page (0);
  if (args_copy == NULL)
    return TID_ERROR;
  strlcpy (args_copy, args, PGSIZE);

  /* Prepare a cl_args to pass to start_process. */
  cl_args = (struct cl_args *) palloc_get_page (0);
  if (cl_args == NULL)
    return TID_ERROR;
  cl_args->argc = 0;
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
  }

  /* TODO This should be handled more cleanly than with an ASSERT. */
  ASSERT (0 < cl_args->argc);

  /* Free our working memory. */
  palloc_free_page (args_copy);

  /* Create a new thread to execute FILE_NAME. */
  const char *file_name = cl_args->args;
  tid = thread_create (file_name, PRI_DEFAULT, start_process, cl_args);

  /* start_process will free cl_args if it starts OK. 
     If not, we have to clean this up ourselves. */
  if (tid == TID_ERROR)
    palloc_free_page (cl_args); 

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
   running with its args. Does not return. */
static void
start_process (void *cl_args_)
{
  ASSERT (cl_args_ != NULL);

  int i;
  struct cl_args *cl_args = cl_args_;
  char *file_name = cl_args->args;
  struct intr_frame if_;
  bool success;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (file_name, &if_.eip, &if_.esp);

  /* Now esp is the stack pointer. Load up the arguments. */
  struct int64_elem *ie = NULL; 

  /* Calculate the starting offset of each string. */
  struct list offset_list;
  list_init (&offset_list);
  int64_t off = 0;
  for (i = 0; i < cl_args->argc; i++)
  {
    ie = (struct int64_elem *) malloc(sizeof(struct int64_elem));
    ie->val = off;
    list_push_back (&offset_list, &ie->elem);
    /* Skip to the next string. */
    off += strlen (cl_args->args + off) + 1;
  }

  char *orig_stack_top = (char *) if_.esp;

  /* Push arguments onto stack in reverse order. */
  char *curr = NULL;
  struct list_elem *e = NULL;
  for (e = list_rbegin (&offset_list); e != list_rend (&offset_list);
       e = list_prev (e))
  {
    /* Calculate offset of the next arg to push. */
    ie = list_entry (e, struct int64_elem, elem);
    curr = cl_args->args + ie->val;
    stack_push_string (&if_.esp, curr);
  }
  /* Round down to multiple of 4. */
  stack_align (&if_.esp, 4);
  /* Push a null pointer sentinel. */
  stack_push_ptr (&if_.esp, NULL);

  /* Push the location of each of the args I pushed above. */
  char *curr_sp = orig_stack_top;
  for (e = list_rbegin (&offset_list); e != list_rend (&offset_list);
       e = list_prev (e))
  {
    /* Calculate offset of this arg. */
    ie = list_entry (e, struct int64_elem, elem);
    curr = cl_args->args + ie->val;
    curr_sp -= strlen (curr) + 1;
    stack_push_ptr (&if_.esp, (void *) curr_sp);
  }

  /* Push the location of argv itself, which is located at
     the current location of the stack pointer. */
  curr_sp = (char *) if_.esp;
  stack_push_ptr (&if_.esp, curr_sp);

  /* Push argc. */
  stack_push_int (&if_.esp, cl_args->argc);
  /* Push fake return address. */
  stack_push_ptr (&if_.esp, NULL);

  /* Clean up memory. */
  palloc_free_page (cl_args);
  while (!list_empty (&offset_list))
  {
    struct list_elem *e = list_pop_front (&offset_list);
    ie = list_entry (e, struct int64_elem, elem);
    free(ie);
  }

  /* If load failed, quit. */
  if (!success) 
    thread_exit ();

  /* 3.3.5: Lock writes to the executable while we are running. 
   
     Closing a file will re-enable writes.

     TODO Verify that we close open files on our way out. 
     This is done in process_exit; do we go through there
     when we die? */
  filesys_lock ();
  int fd = thread_new_file (file_name);
  if (0 <= fd)
  {
    struct file *file_obj = thread_fd_lookup (fd);
    ASSERT (file_obj != NULL);
    file_deny_write (file_obj);
  }
  filesys_unlock ();
  
  /* If we couldn't open the file, quit. Something is quite wrong. */
  if(fd < 0)
    thread_exit ();

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid UNUSED) 
{
  /* TODO Infinite loop for now. */
  for(;;) ; 
  return -1;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  /* Close all open file handles and free the memory. */
  file_table_destroy (&cur->fd_table);

  /* TODO Unlock any locks we hold? Or does this only matter
     for kernel threads? Can a thread hold locks that are unsafe
     to leave locked? This would require shared memory, or the ability
     to lock some kernel object... */
  ASSERT (list_empty (&cur->lock_list));

  /* Tell parent that we're exiting.
     Exit status must have been set already (done in syscall_exit). */
  process_mark_exiting (thread_get_child_info_self ()->child_exit_status);

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

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

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
  file_close (file);
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
