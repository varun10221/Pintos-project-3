#include "userprog/syscall.h"

#include <stdio.h>
#include <stdbool.h>
#include <syscall-nr.h>
#include <limits.h>
#include <string.h>
#include "userprog/stack.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "lib/user/syscall.h"
#include "vm/page.h"

/* Size of tables. */
#define N_SUPPORTED_SYSCALLS 15

/* Read/write buffer size. We do IO in this unit. */
#define STDINOUT_BUFSIZE 512

enum io_type 
  {
    IO_TYPE_READ, /* IO will read. */
    IO_TYPE_WRITE /* IO will write. */
  };

/* For testing user-provided addresses. */
static bool maybe_valid_uaddr (const void *uaddr);
static bool is_valid_uptr (void *u_ptr);
static bool is_valid_ustring (const char *u_str);
static bool is_valid_ubuf (void *u_buf, unsigned size, bool will_write);

static bool strncpy_from_user (char *dest, const char *src, unsigned MAX_LEN);
static bool copy_from_user (void *dest, const void *src, unsigned n_bytes);
static bool copy_ptr_from_user (void *dest, const void *src);
static bool copy_int32_t_from_user (int32_t *dest, const int32_t *src);

static bool pop_int32_t_from_user_stack (int32_t *dest, int32_t **stack);
static bool pop_ptr_from_user_stack (void *dest, void **stack);

static int get_user (const uint8_t *uaddr);
static bool put_user (uint8_t *udst, uint8_t byte);
static bool read_user_str (const char *str);

/* Number of args for each syscall.
   Table is indexed by SYSCALL_* value from lib/syscall-nr.h */
static int syscall_nargs[N_SUPPORTED_SYSCALLS] =
{
0 /* halt */,   1 /* exit */,
1 /* exec */,   1 /* wait */,
2 /* create */, 1 /* remove */,
1 /* open */,   1 /* filesize */,
3 /* read */,   3 /*write */,
2 /* seek */,   1 /* tell */,
1 /* close */,  2 /* mmap */,
1 /* munmap */
};

/* Whether or not the syscall has a user-provided address.
   Table is indexed by SYSCALL_* value from lib/syscall-nr.h */
static bool syscall_has_pointer[N_SUPPORTED_SYSCALLS] =
{
false /* halt */,   false /* exit */,
true /* exec */,    false /* wait */,
true /* create */,  true /* remove */,
true /* open */,    false /* filesize */,
true /* read */,    true /*write */,
false /* seek */,   false /* tell */,
false /* close */,  true /* mmap */,
false /* munmap */
};

static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void syscall_halt (void);
static void syscall_exit (int status);
static pid_t syscall_exec (const char *cmd_line);
static int syscall_wait (pid_t pid);
static bool syscall_create (const char *file, unsigned initial_size);
static bool syscall_remove (const char *file);
static int syscall_open (const char *file);
static int syscall_filesize (int fd);
static int syscall_read (int fd, void *buffer, unsigned size);
static int syscall_write (int fd, const void *buffer, unsigned size);
static void syscall_seek (int fd, unsigned position);
static unsigned syscall_tell (int fd);
static void syscall_close (int fd);
static mapid_t syscall_mmap (int fd, void *addr);
static void syscall_munmap (mapid_t mapping);

/* Handle the syscall contained in this stack frame.
   Any invalid pointers (stack pointer itself, or values
   referred to therein) will prompt exit(-1). 
   
   TODO Is this the right place to do this? I think so, look at e.g. syscall3.
     As this is the "first" place we come on transition from
     user mode to kernel mode, save the stack pointer to the thread
     in case we need to grow the stack in kernel mode (see tests/vm/pt-grow-stk-sc.c). */
static void
syscall_handler (struct intr_frame *f) 
{
  int i;

  /* Assume we fail. */
  f->eax = -1;

  /* Copy esp so we can navigate the stack without disturbing the caller. */
  void *sp = f->esp;
  /* Copy esp for handling stack growth in kernel mode. */
  struct thread *thr = thread_current ();
  thr->vm_esp = f->esp;
  thr->is_handling_syscall = true;

  /* Identify syscall. */
  int32_t syscall;
  if (!pop_int32_t_from_user_stack (&syscall, &sp))
    syscall_exit (-1);

  /* Invalid syscall? */
  if (syscall < SYS_MIN || SYS_MAX < syscall)
    syscall_exit(-1);

  /* Extract the args. */
  int32_t args[3]; 
  for (i = 0; i < syscall_nargs[syscall - SYS_MIN]; i++)
  {
    if (!pop_int32_t_from_user_stack (&args[i], &sp))
      syscall_exit (-1);
  }

  /* For those syscalls that evaluate a user-provided address,
     test here for safety. This allows us to centralize the error handling
     and lets us simplify the syscall_* routines. 
     
     If an address is invalid, we change the syscall to SYS_EXIT to terminate
     the program. */
  if (syscall_has_pointer[syscall - SYS_MIN])
  {
    /* One of ustr, ubuf, and uaddr will be set to non-NULL (unless user passed a NULL pointer...) */
    char *ustr = NULL, *ubuf = NULL, *uaddr = NULL;
    unsigned length = 0;
    enum io_type io_t;
    switch (syscall)
    {
      case SYS_EXEC:                   /* Start another process. */
        ustr = (char *) args[0];
        break;
      case SYS_CREATE:                 /* Create a file. */
        ustr = (char *) args[0];
        break;
      case SYS_REMOVE:                 /* Delete a file. */
        ustr = (char *) args[0];
        break;
      case SYS_OPEN:                   /* Open a file. */
        ustr = (char *) args[0];
        break;
      case SYS_READ:                   /* Read from a file. */
        ubuf = (void *) args[1];
        length = (unsigned) args[2];
        io_t = IO_TYPE_READ;
        break;
      case SYS_WRITE:                  /* Write to a file. */
        ubuf = (void *) args[1];
        length = (unsigned) args[2];
        io_t = IO_TYPE_WRITE;
        break;
      case SYS_MMAP:                   /* Map a file into memory. */
        uaddr = (void *) args[1]; 
        break;
      default:
        NOT_REACHED ();
    }

    if (ustr != NULL)
    {
      if (!is_valid_ustring (ustr))
      {
        /* Exit in error. */ 
        syscall = SYS_EXIT;
        args[0] = -1;
      }
    }
    else if (uaddr != NULL)
    {
      if (!is_valid_uptr (uaddr))
      {
        /* Exit in error. */ 
        syscall = SYS_EXIT;
        args[0] = -1;
      }
    }
    else if (ubuf != NULL)
    {
      /* On a READ syscall we will write to the user-provided buffer. */
      if (!is_valid_ubuf (ubuf, length, io_t == IO_TYPE_READ))
      {
        /* Exit in error. Use -1 to match API of wait(). */
        syscall = SYS_EXIT;
        args[0] = -1;
      }
    }
    /* OK, both buffers are null. This may or may not be OK. */
    else if ( (syscall == SYS_READ || syscall == SYS_WRITE) && length == 0 )
    {
      /* User wants to do IO with length 0. Success, I guess. */
      f->eax = 0;
      return;
    }
    else
    {
      /* Both buffers are null, meaning that the user provided a null address.
         He did not request anything trivial that we can satisfy.
         Exit in error. */
      syscall = SYS_EXIT;
      args[0] = -1;
    }
  } /* End of user-provided address handling. */

  /* Ready to handle whatever syscall they requested. */
  switch (syscall)
  {
    case SYS_HALT:                   /* Halt the operating system. */
      syscall_halt ();
      NOT_REACHED ();
      break;
    case SYS_EXIT:                   /* Terminate this process. */
      syscall_exit ((int) args[0]);
      NOT_REACHED ();
      break;
    case SYS_EXEC:                   /* Start another process. */
      f->eax = syscall_exec ((char *) args[0]);
      break;
    case SYS_WAIT:                   /* Wait for a child process to die. */
      f->eax = syscall_wait ((pid_t) args[0]);
      break;
    case SYS_CREATE:                 /* Create a file. */
      f->eax = syscall_create ((char *) args[0], (unsigned) args[1]);
      break;
    case SYS_REMOVE:                 /* Delete a file. */
      f->eax = (syscall_remove ((char *) args[0]) ? 1 : 0);
      break;
    case SYS_OPEN:                   /* Open a file. */
      f->eax = syscall_open ((char *) args[0]);
      break;
    case SYS_FILESIZE:               /* Obtain a file's size. */
      f->eax = syscall_filesize ((int) args[0]);
      break;
    case SYS_READ:                   /* Read from a file. */
      f->eax = syscall_read ((int) args[0], (void *) args[1], (unsigned) args[2]);
      break;
    case SYS_WRITE:                  /* Write to a file. */
      f->eax = syscall_write ((int) args[0], (void *) args[1], (unsigned) args[2]);
      break;
    case SYS_SEEK:                   /* Change position in a file. */
      syscall_seek ((int) args[0], (unsigned) args[1]);
      f->eax = 0; /* void */
      break;
    case SYS_TELL:                   /* Report current position in a file. */
      f->eax = syscall_tell ((int) args[0]);
      break;
    case SYS_CLOSE:                  /* Close a file. */
      syscall_close ((int) args[0]);
      f->eax = 0; /* void */
      break;

    /* Project 3 and optionally project 4. */
    case SYS_MMAP:                   /* Map a file into memory. */
      f->eax = syscall_mmap ((int) args[0], (void *) args[1]);
      break;
    case SYS_MUNMAP:                 /* Remove a memory mapping. */
      syscall_munmap ((mapid_t) args[0]);
      f->eax = 0; /* void */
      break;

    /* Un-implemented syscalls: drop through to the printf. */
    /* Project 4 only. */
    case SYS_CHDIR:                  /* Change the current directory. */
    case SYS_MKDIR:                  /* Create a directory. */
    case SYS_READDIR:                /* Reads a directory entry. */
    case SYS_ISDIR:                  /* Tests if a fd represents a directory. */
    case SYS_INUMBER:                 /* Returns the inode number for a fd. */

    default:
      printf ("Unsupported system call! vec %i esp %i\n", f->vec_no, *(int32_t *) f->esp);
  };  

  thr->is_handling_syscall = false;
}

/* Any user-provided addresses given to the syscall_* routines have
   already been tested for correctness.

   The use of an invalid fd produces undefined behavior. */

/* Terminates Pintos by calling shutdown_power_off(). */
static void 
syscall_halt ()
{
  shutdown_power_off ();
}

/* Terminates the current user program, returning status to the kernel. 
   If the process's parent waits for it (see below), this is the status that 
   will be returned. Conventionally, a status of 0 indicates success and nonzero
   values indicate errors. */
static void 
syscall_exit (int status)
{
  /* Mark our exit status. */
  process_set_exit_status (status);

  thread_exit ();
  NOT_REACHED ();
}

/* Runs the executable whose name is given in cmd_line, passing any given arguments, 
   and returns the new process's program id (pid). Must return pid -1, which 
   otherwise should not be a valid pid, if the program cannot load or run for any 
   reason. Thus, the parent process will not return from the exec until it knows 
   whether the child process successfully loaded its executable. */
static pid_t 
syscall_exec (const char *cmd_line)
{
  ASSERT (cmd_line != NULL);

  tid_t child = process_execute (cmd_line);
  return child;
}

/* Waits for a child process pid and retrieves the child's exit status.
   If pid is still alive, waits until it terminates. */
static int 
syscall_wait (pid_t pid)
{
  return process_wait (pid);
}

/* Creates a new file called file initially initial_size bytes in size. 
   Returns true if successful, false otherwise. */
static bool 
syscall_create (const char *file, unsigned initial_size)
{
  ASSERT (file != NULL);

  /* Copy file into process's scratch page so that it will be safe from page fault. */
  char *sp = (char *) process_get_scratch_page ();
  ASSERT (PATH_MAX <= PGSIZE);
  size_t len = strlcpy (sp, file, PGSIZE);
  /* If too long, fail gracefully (a la ENAMETOOLONG). */
  if (PATH_MAX < len+1) 
    return false;

  bool success = false;
  filesys_lock ();
  success = filesys_create (sp, initial_size);
  filesys_unlock ();
  return success;
}

/* Deletes the file called file. Returns true if successful, false otherwise. */
static bool 
syscall_remove (const char *file)
{
  ASSERT (file != NULL);

  /* Copy file into process's scratch page so that it will be safe from page fault. */
  char *sp = (char *) process_get_scratch_page ();
  ASSERT (PATH_MAX <= PGSIZE);
  size_t len = strlcpy (sp, file, PGSIZE);
  /* If too long, fail gracefully (a la ENAMETOOLONG). */
  if (PATH_MAX < len+1) 
    return false;

  bool success = false;
  filesys_lock ();
  /* Note that this has no effect on outstanding fds in this process's fd_table. */
  success = filesys_remove (sp);
  filesys_unlock ();
  return success;
}

/* Opens the file called file. 
   Returns a nonnegative integer handle called a "file descriptor" (fd), 
   or -1 if the file could not be opened. */
static int 
syscall_open (const char *file)
{
  ASSERT (file != NULL);

  /* Copy file into process's scratch page so that it will be safe from page fault. */
  char *sp = (char *) process_get_scratch_page ();
  ASSERT (PATH_MAX <= PGSIZE);
  size_t len = strlcpy (sp, file, PGSIZE);
  /* If too long, fail gracefully (a la ENAMETOOLONG). */
  if (PATH_MAX < len+1) 
    return -1;

  filesys_lock ();
  int fd = process_new_file (sp);
  filesys_unlock ();
  return fd;
}

/* Returns the size, in bytes, of the file open as fd. */
static int 
syscall_filesize (int fd)
{
  int rc = -1;
  filesys_lock ();
  struct file *f = process_fd_lookup (fd);
  if (f != NULL)
    rc = file_length (f);
  filesys_unlock ();
  return rc;
}

/* Reads size bytes from the file open as fd into buffer. 
   Returns the number of bytes actually read (0 at end of file), 
   or -1 if the file could not be read (due to a condition other than end of file). 
  
   Fd 0 reads from the keyboard. */
static int 
syscall_read (int fd, void *buffer, unsigned size)
{
  bool is_valid_fd = (fd == STDIN_FILENO || N_RESERVED_FILENOS <= fd);
  if (!is_valid_fd)
    return -1;

  ASSERT (buffer != NULL);

  int n_left = (int) size;
  int n_read = 0;

  if (fd == STDOUT_FILENO)
  {
    int i;
    char *buf = (char *) buffer;
    for (i = 0; i < n_left; i++)
      buf[i] = (char) input_getc ();
  }
  else
  {
    /* Get the corresponding file. */
    struct file *f = process_fd_lookup (fd);
    if (f == NULL)
      /* We don't have this fd open. */
      return -1;
  
    n_left = (int) size;
    /* NB This implementation makes for interesting potential "races" with other readers/writers. 
       See Piazza, but basically the FS makes no atomicity guarantees in the presence of races. */
    while (0 <= n_left)
    {
      /* For each page in buffer, pin the page to a frame so that we cannot
         page fault while we are holding filesys_lock(). */
      struct page *pg = process_page_table_find_page (buffer);
      ASSERT (pg != NULL);
      process_pin_page (pg);

      int amount_to_read = PGSIZE;
      /* If buffer is not page-aligned, only read up to the end of the page so that 
         subsequent non-final reads will be page-aligned. */
      uint32_t mod_PGSIZE = (uint32_t) buffer % PGSIZE;
      if (mod_PGSIZE != 0)
        /* (read from buffer to the end of its containing page) */
        amount_to_read = PGSIZE - mod_PGSIZE;
      /* n_left is an upper bound on the amount to read. */
      if (n_left < amount_to_read)
        amount_to_read = n_left;

      filesys_lock ();
      n_read = file_read (f, buffer, amount_to_read);
      filesys_unlock ();

      process_unpin_page (pg);

      /* Advance buffer. */
      buffer += n_read;
      n_left -= n_read;
      /* If we read non-negative value but less than the expected amount, we've reached end of file. */
      ASSERT (0 <= n_read && n_read <= amount_to_read);
      if (n_read != amount_to_read)
        break;
    }
  }

  return n_read;
}

/* Writes size bytes from buffer to the open file fd. 
   Returns the number of bytes actually written,
   which may be less than size if some bytes could not be written.  

   Fd 1 writes to the console. */
static int 
syscall_write (int fd, const void *buffer, unsigned size)
{
  bool is_valid_fd = (fd == STDOUT_FILENO || N_RESERVED_FILENOS <= fd);
  if (!is_valid_fd)
    return -1;

  ASSERT (buffer != NULL);

  int n_to_write;
  int n_left = (int) size;
  int n_written = 0;

  if (fd == STDOUT_FILENO)
  {
    while (n_left)
    {
      /* Write no more than STDINOUT_BUFSIZE at a time. */
      n_to_write = (STDINOUT_BUFSIZE <= n_left ? STDINOUT_BUFSIZE : n_left);
      putbuf (buffer + n_written, n_to_write);
      n_written += n_to_write;
      n_left -= n_to_write;
    }
  }
  else
  {
    /* Get the corresponding file. */
    struct file *f = process_fd_lookup (fd);
    if (f == NULL)
      /* We don't have this fd open. */
      return -1;
  
    n_left = (int) size;
    /* NB This implementation makes for interesting potential "races" with other readers/writers. 
       See Piazza, but basically the FS makes no atomicity guarantees in the presence of races. */
    while (0 <= n_left)
    {
      /* For each page in buffer, pin the page to a frame so that we cannot
         page fault while we are holding filesys_lock(). */
      struct page *pg = process_page_table_find_page (buffer);
      ASSERT (pg != NULL);
      process_pin_page (pg);

      int amount_to_write = PGSIZE;
      /* If buffer is not page-aligned, only write up to the end of the page so that 
         subsequent non-final writes will be page-aligned. */
      uint32_t mod_PGSIZE = (uint32_t) buffer % PGSIZE;
      if (mod_PGSIZE != 0)
        /* (write from buffer to the end of its containing page) */
        amount_to_write = PGSIZE - mod_PGSIZE;
      /* n_left is an upper bound on the amount to write. */
      if (n_left < amount_to_write)
        amount_to_write = n_left;

      filesys_lock ();
      n_written = file_write (f, buffer, amount_to_write);
      filesys_unlock ();

      process_unpin_page (pg);

      /* Advance buffer. */
      buffer += n_written;
      n_left -= n_written;
      /* If we write non-negative value but less than the expected amount, we've reached end of file. */
      ASSERT (0 <= n_written && n_written <= amount_to_write);
      if (n_written != amount_to_write)
        break;
    }
  }

  return n_written;
}

/* Changes the next byte to be read or written in open file fd to position, 
   expressed in bytes from the beginning of the file. */
static void 
syscall_seek (int fd, unsigned position)
{
  filesys_lock ();
  struct file *f = process_fd_lookup (fd);
  if (f != NULL)
    file_seek (f, position);
  filesys_unlock ();
  return;
}

/* Returns the position of the next byte to be read or written in open file fd,
   expressed in bytes from the beginning of the file. */
static unsigned 
syscall_tell (int fd)
{
  unsigned rc = 0;

  filesys_lock ();
  struct file *f = process_fd_lookup (fd);
  if (f != NULL)
    rc = file_tell (f);
  filesys_unlock ();
  
  return rc;
}

/* Closes file descriptor fd. */
static void 
syscall_close (int fd)
{
  filesys_lock ();
  process_fd_delete (fd);
  filesys_unlock ();
  return;
}

/* Maps the file open as FD into the process's virtual address space at ADDR. 
 
   Returns a "mapping ID" that uniquely identifies the mapping within
   the process. Returns -1 on failure. */
static mapid_t syscall_mmap (int fd, void *addr)
{
  struct file *f = NULL;
  struct mmap_info *mmap_info = NULL;
  mapid_t mapping = -1;

  f = process_fd_lookup (fd);
  if (f == NULL)
    goto CLEANUP_AND_ERROR;

  mmap_info = process_add_mapping (f, addr, MAP_PRIVATE|MAP_RDWR);
  /* TODO Just experimenting with shared mmap.
  mmap_info = process_add_mapping (f, addr, MAP_SHARED|MAP_RDWR); */
  if (mmap_info == NULL)
    goto CLEANUP_AND_ERROR;

  mapping = process_mmap_add (mmap_info);
  if (mapping == -1)
    goto CLEANUP_AND_ERROR;

  return mapping;

  CLEANUP_AND_ERROR:
    if (mmap_info != NULL)
      /* process_delete_mapping will close f_dup. */
      process_delete_mapping (mmap_info);
    return -1;
}

/* Unmaps the mapping designated by MAPPING. */
static void syscall_munmap (mapid_t mapping)
{
  struct mmap_info *mmap_info = process_mmap_lookup (mapping);

  /* No such mapping. */
  if (mmap_info == NULL)
    return;

  process_delete_mapping (mmap_info);
  process_mmap_remove (mapping);
}

/* Returns true if UADDR is non-null and below kernel space. 
   UADDR may point to un-mapped user space. */
static bool 
maybe_valid_uaddr (const void *uaddr)
{
  return (uaddr != NULL && is_user_vaddr (uaddr));
}

/* Returns true if we can read user-provided c-string U_STR
   without pagefaulting or leaving the user's legal address space. */
static bool 
is_valid_ustring (const char *u_str)
{
  return read_user_str (u_str);
}

/* Returns true if we can read user-space U_PTR
   without pagefaulting or leaving the user's legal address space. */
static bool 
is_valid_uptr (void *u_ptr)
{
  return is_valid_ubuf (u_ptr, sizeof(void *), false);
}

/* Copy this many bytes from user-space SRC into DEST.
   True on success, false if we leave user's legal address space or pagefault. */
static bool 
copy_from_user (void *dest, const void *src, unsigned n_bytes)
{
  ASSERT (dest != NULL);

  int rc;
  unsigned i;
  for (i = 0; i < n_bytes; i++)
  {
    if (!maybe_valid_uaddr(src) || (rc = get_user(src)) == -1)
      return false;
    *(char *) dest = rc;
    dest++;
    src++;
  }

  return true;
}

/* Copy a pointer's worth of data from user-space SRC into DEST.
   True on success, false if we leave user's legal address space or pagefault. */
static bool 
copy_ptr_from_user (void *dest, const void *src)
{
  return copy_from_user (dest, src, sizeof(void *));
}

/* Copy a pointer's worth of data from user-space STACK into DEST.
   Pop STACK on success. 
   True on success, false if we leave user's legal address space or pagefault. */
static bool
pop_ptr_from_user_stack (void *dest, void **stack)
{
  ASSERT (dest != NULL);
  ASSERT (stack != NULL);

  bool success = copy_ptr_from_user (dest, *stack);
  if (success)
    stack_pop (stack);
  return success;
}

/* Copy an int32_t from SRC into DEST.
   True on success, false if we leave user's legal address space or pagefault. */
static bool 
copy_int32_t_from_user (int32_t *dest, const int32_t *src)
{
  return copy_from_user (dest, src, sizeof(int32_t));
}

/* Copy an int32_t from SRC into DEST.
   Pop STACK on success. 
   True on success, false if we leave user's legal address space or pagefault. */
static bool 
pop_int32_t_from_user_stack (int32_t *dest, int32_t **stack)
{
  ASSERT (dest != NULL);
  ASSERT (stack != NULL);

  bool success = copy_int32_t_from_user (dest, *stack);
  if (success)
    stack_pop (stack);
  return success;
}

/* Copy string from user-space SRC into DEST. 
   True on success, false if we leave user's legal address space or pagefault. 
   False if src is more than MAX_LEN characters, including terminating null byte. 
   
   On failure, dest may contain up to the first MAX_LEN characters of u_str,
   and in this case is not a valid c-string. 
   
   Not used in P2, but will be needed in P3. */
static bool 
strncpy_from_user (char *dest, const char *src, unsigned MAX_LEN)
{
  ASSERT (dest != NULL);

  int rc;
  int i;
  for (i = 0; i < MAX_LEN; i++)
  {
    if (!maybe_valid_uaddr (src) || (rc = get_user (src)) == -1)
      return false;
    *dest = rc;
    /* Did we read the null byte? */
    if (rc == '\0')
      return true;
    dest++;
    src++;
  }

  /* We have read MAX_LEN characters, so the string is too long for DEST. */
  return false;
}

/* Returns true if we can read or write user-provided buffer U_BUF
   without pagefaulting or leaving the user's legal address space. 
   
   The buffer is unmodified (if WILL_WRITE, we read a byte and 
   then write the byte back). */
static bool 
is_valid_ubuf (void *u_buf, unsigned size, bool will_write)
{
  int rc;
  void *p = u_buf;

  /* Since memory is paged, we only need to test the first byte and the first 
     byte of every subsequent page. The same tests are performed
     prior to the loop on p and in the loop on the first byte of subsequent pages. */

  /* Valid address and we can read without segfaulting? */
  if (!maybe_valid_uaddr(p) || (rc = get_user (p)) == -1)
    return false;
  /* User requested a read, so we need to be able to write to this byte.
     Try to write the byte we just read so we don't pollute the user's buffer
     in the event of a read failure. */
  if (will_write && !put_user (p, rc))
    return false;

  /* Last byte we need to test. */
  void *last_byte = u_buf + size - 1;

  /* Loop until we pass the last_byte. */
  p = pg_round_up (p);
  while (p <= last_byte)
  {
    /* Same tests as above. */
    if (!maybe_valid_uaddr(p) || (rc = get_user (p)) == -1)
      return false;
    if (will_write && !put_user (p, rc))
      return false;
    p = pg_round_up (p+1);
  }

  return true;
}

/* Code from Dr. Back to allow faster user address checking. */

/* Reads a byte at user virtual address UADDR.
   UADDR must be below PHYS_BASE.
   Returns the byte value if successful, -1 if a segfault
   occurred. */
static int
get_user (const uint8_t *uaddr)
{
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return result;
}

/* Read each byte of user-provided STR.
   Returns true if successful, false if segfault. */
static bool
read_user_str (const char *str)
{
  bool did_segfault = false;
  int rc;
  do{
    /* Non-NULL and below kernel space? */
    if (!maybe_valid_uaddr (str))
    {
      did_segfault = true;
      break;
    }

    /* Is in a mapped page? */
    rc = get_user ((uint8_t *) str);
    if (rc == -1)
    {
      did_segfault = true;
      break;
    }

    str++;
  } while (rc != '\0');

  return !did_segfault;
}

/* Writes BYTE to user address UDST.
   UDST must be below PHYS_BASE.
   Returns true if successful, false if a segfault occurred. */
static bool
put_user (uint8_t *udst, uint8_t byte)
{
   int error_code;
   asm ("movl $1f, %0; movb %b2, %1; 1:"
        : "=&a" (error_code), "=m" (*udst) : "q" (byte));
   return error_code != -1;
}

/* Human-readable syscall. Useful for debugging syscall_handler. */
static char *
syscall_to_str (int syscall)
{
  ASSERT (SYS_MIN <= syscall && syscall <= SYS_MAX);

  switch (syscall)
  {
    case SYS_HALT:
      return "SYS_HALT";
    case SYS_EXIT:
      return "SYS_EXIT";
    case SYS_EXEC:
      return "SYS_EXEC";
    case SYS_WAIT:
      return "SYS_WAIT";
    case SYS_CREATE:
      return "SYS_CREATE";
    case SYS_REMOVE:
      return "SYS_REMOVE";
    case SYS_OPEN:
      return "SYS_OPEN";
    case SYS_FILESIZE:
      return "SYS_FILESIZE";
    case SYS_READ:
      return "SYS_READ";
    case SYS_WRITE:
      return "SYS_WRITE";
    case SYS_SEEK:
      return "SYS_SEEK";
    case SYS_TELL:
      return "SYS_TELL";
    case SYS_CLOSE:
      return "SYS_CLOSE";

    /* Project 3 and optionally project 4. */
    case SYS_MMAP:
      return "SYS_MMAP";
    case SYS_MUNMAP:
      return "SYS_MUNMAP";

    /* Project 4 only. */
    case SYS_CHDIR:
      return "SYS_CHDIR";
    case SYS_MKDIR:
      return "SYS_MKDIR";
    case SYS_READDIR:
      return "SYS_READDIR";
    case SYS_ISDIR:
      return "SYS_ISDIR";
    case SYS_INUMBER:
      return "SYS_INUMBER";
    default:
      NOT_REACHED ();
  }
}
