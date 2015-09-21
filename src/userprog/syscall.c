#include "userprog/syscall.h"

#include <stdio.h>
#include <stdbool.h>
#include <syscall-nr.h>
#include "userprog/stack.h"
#include "userprog/pagedir.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "lib/user/syscall.h"

/* Size of tables. */
#define N_SUPPORTED_SYSCALLS 13

/* Read/write buffer size. We do IO in this unit. */
#define STDINOUT_BUFSIZE 512

enum io_type 
  {
    IO_TYPE_READ, /* IO will read. */
    IO_TYPE_WRITE /* IO will write. */
  };

/* For testing user-provided addresses. */
static bool is_valid_uaddr (uint32_t *pd, const void *uaddr);
static bool is_valid_ustring (uint32_t *pd, const char *u_str);
static bool is_valid_ubuf (uint32_t *pd, const void *u_buf, unsigned size, enum io_type io_t);
static int get_user (const uint8_t *uaddr);
static bool put_user (uint8_t *udst, uint8_t byte);

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
1 /* close */
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
false /* close */
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

static void
syscall_handler (struct intr_frame *f) 
{
  /* printf ("system call! vec %i esp %p\n", f->vec_no, f->esp); */

  int i;

  /* Assume we fail. */
  f->eax = -1;

  /* Get this thread's pagedir so we can check pointers for validity. */
  uint32_t *pd = thread_current ()->pagedir;
  /* Copy esp so we can navigate the stack without disturbing the caller. */
  void *sp = f->esp;
  /* Identify syscall. */
  int32_t syscall = *(int32_t *) stack_pop (&sp);

  ASSERT (SYS_MIN <= syscall && syscall <= SYS_MAX);

  /* Extract the args. */

  /* No syscall takes more than 3 args. All args on stack are 4 bytes; cast as appropriate. */
  int32_t args[3]; 
  for(i = 0; i < syscall_nargs[syscall - SYS_MIN]; i++)
    args[i] = *(int32_t *) stack_pop (&sp);

  /* For those syscalls that evaluate a user-provided address,
     test here for safety. This allows us to centralize the error handling
     and lets us simply the syscall_* routines. 
     
     If an address is invalid, we change the syscall to SYS_EXIT to terminate
     the program. */
  if (syscall_has_pointer[syscall - SYS_MIN])
  {
    /* Either ustr or ubuf will be set to non-NULL. */
    char *ustr = NULL, *ubuf = NULL;
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
      default:
        NOT_REACHED ();
    }

    ASSERT (ustr != NULL || ubuf != NULL); 
    if (ustr != NULL)
    {
      if (!is_valid_ustring (pd, ustr))
      {
        /* Exit in error. Use -1 to match API of wait(). */
        syscall = SYS_EXIT;
        args[0] = -1;
      }
    }
    else if (ubuf != NULL)
    {
      if (!is_valid_ubuf (pd, ubuf, length, io_t))
      {
        /* Exit in error. Use -1 to match API of wait(). */
        syscall = SYS_EXIT;
        args[0] = -1;
      }
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

    /* Un-implemented syscalls: drop through to the printf. */

    /* Project 3 and optionally project 4. */
    case SYS_MMAP:                   /* Map a file into memory. */
      /* TODO */
    case SYS_MUNMAP:                 /* Remove a memory mapping. */
      /* TODO */

    /* Project 4 only. */
    case SYS_CHDIR:                  /* Change the current directory. */
      /* TODO */
    case SYS_MKDIR:                  /* Create a directory. */
      /* TODO */
    case SYS_READDIR:                /* Reads a directory entry. */
      /* TODO */
    case SYS_ISDIR:                  /* Tests if a fd represents a directory. */
      /* TODO */
    case SYS_INUMBER:                 /* Returns the inode number for a fd. */
    default:
      printf ("Unsupported system call! vec %i esp %i\n", f->vec_no, *(int32_t *) f->esp);
  };  
}

/* Any user-provided addresses given to the syscall_* routines have
   already been tested for correctness.

   The use of an invalid fd produces undefined behavior. */

/* Terminates Pintos by calling shutdown_power_off(). */
static void 
syscall_halt ()
{
  /* TODO Anything else? */
  shutdown_power_off ();
}

/* Terminates the current user program, returning status to the kernel. 
   If the process's parent waits for it (see below), this is the status that 
   will be returned. Conventionally, a status of 0 indicates success and nonzero
   values indicate errors. */
static void 
syscall_exit (int status)
{
  /* TODO */
  return;
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
  /* TODO */
  return 0;
}

/* Waits for a child process pid and retrieves the child's exit status.
   If pid is still alive, waits until it terminates. */
static int 
syscall_wait (pid_t pid)
{
  /* TODO */
  return -1;
}

/* Creates a new file called file initially initial_size bytes in size. 
   Returns true if successful, false otherwise. */
static bool 
syscall_create (const char *file, unsigned initial_size)
{
  ASSERT (file != NULL);

  bool success = false;
  filesys_lock ();
  success = filesys_create (file, initial_size);
  filesys_unlock ();
  return success;
}

/* Deletes the file called file. Returns true if successful, false otherwise. */
static bool 
syscall_remove (const char *file)
{
  ASSERT (file != NULL);

  bool success = false;
  filesys_lock ();
  /* Note that this has no effect on outstanding fd's in this process's fd_table. */
  success = filesys_remove (file);
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

  filesys_lock ();
  int fd = thread_new_file (file);
  filesys_unlock ();
  return fd;
}

/* Returns the size, in bytes, of the file open as fd. */
static int 
syscall_filesize (int fd)
{
  int rc = -1;
  filesys_lock ();
  struct file *f = thread_fd_lookup (fd);
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

  unsigned n_left = size;
  unsigned n_read = 0;

  if (fd == STDOUT_FILENO)
  {
    unsigned i;
    char *buf = (char *) buffer;
    for (i = 0; i < n_left; i++)
    {
      buf[i] = (char) input_getc ();
    }
  }
  else
  {
    n_read = 0;
    filesys_lock ();
    struct file *f = thread_fd_lookup (fd);
    if (f == NULL)
      /* We don't have this fd open. */
      return -1;
    else
      /* Let file_read do any desired buffering. */
      n_read = file_read (f, buffer, size);
    filesys_unlock ();
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
  if (! is_valid_fd)
    return -1;

  ASSERT (buffer != NULL);

  unsigned n_to_write;
  unsigned n_left = size;
  unsigned n_written = 0;

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
    n_written = 0;
    filesys_lock ();
    struct file *f = thread_fd_lookup (fd);
    if (f == NULL)
      /* We don't have this fd open. */
      return -1;
    else
      /* Let file_write do any desired buffering. */
      n_written = file_write (f, buffer, size);
    filesys_unlock ();
  }

  return n_written;
}

/* Changes the next byte to be read or written in open file fd to position, 
   expressed in bytes from the beginning of the file. */
static void 
syscall_seek (int fd, unsigned position)
{
  filesys_lock ();
  struct file *f = thread_fd_lookup (fd);
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
  struct file *f = thread_fd_lookup (fd);
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
  thread_fd_delete (fd);
  filesys_unlock ();
  return;
}

/* Returns true if uaddr is non-null and points to already-mapped non-kernel address space. */
static bool 
is_valid_uaddr (uint32_t *pd, const void *uaddr)
{
  /* TODO Inefficient, but need clarification from Dr. Back on the use of get_user. */
  return (uaddr != NULL && is_user_vaddr (uaddr) && pagedir_get_page (pd, uaddr) != NULL);
}

/* Returns true if we can read this user-provided c-string 
   without pagefaulting or leaving the user's legal address space. */
static bool 
is_valid_ustring (uint32_t *pd, const char *u_str)
{
  ASSERT (pd != NULL);
  ASSERT (u_str != NULL);

  /* Is the first byte safe? */
  if (!is_valid_uaddr(pd, u_str))
    return false;

  /* The first byte is safe. Check each byte until we reach the
     end of the string or encounter an illegal address. */
  char *p = u_str;

  /* Invariant: *p is a safe address to dereference. */
  while (*p != '\0')
  {
    p++; 
    /* Read this byte and see if it's OK. */
    if (!is_valid_uaddr (pd, p))
      return false;
  }

  return true;
}

/* Returns true if we can read or write this user-provided buffer
   without pagefaulting or leaving the user's legal address space. */
static bool 
is_valid_ubuf (uint32_t *pd, const void *u_buf, unsigned size, enum io_type io_t)
{
  ASSERT (pd != NULL);
  ASSERT (u_buf != NULL);
  ASSERT (io_t == IO_TYPE_READ || IO_TYPE_WRITE);

  /* TODO Speed enhancement: We only need to test the first byte and every subsequent page boundary. */

  /* Test each byte. */
  unsigned i;
  for (i = 0; i < size; i++)
  {
    if (!is_valid_uaddr(pd, u_buf+i))
      return false;
    /* For use with get_char / put_char. */
    /*
    if (io_t == IO_TYPE_WRITE)
    {
      Try a put_char
    }
    else
    {
      Try a get_char
    }
    */
  }

  return true;
}

/* Code to allow faster user address checking. */

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
