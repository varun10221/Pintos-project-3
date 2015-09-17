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
#include "lib/user/syscall.h"

/* For masking with syscall_isptr. */
#define ARG_ONE   (1 << 0)
#define ARG_TWO   (1 << 1)
#define ARG_THREE (1 << 2)

/* Size of tables. */
#define N_SUPPORTED_SYSCALLS 13

/* Read/write buffer size. We do IO in this unit. */
#define IO_BUFSIZE 512

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

/* Represent whether a syscall arg is a pointer or not
   with a mask: is_pointer = (ptr & ARG_{ONE,TWO,THREE}).
   Table is indexed by SYSCALL_* value from lib/syscall-nr.h */
static int syscall_isptr[N_SUPPORTED_SYSCALLS] =
{
0       /* halt */,       0 /* exit */,
ARG_ONE /* exec */,       0 /* wait */,
ARG_ONE /* create */,     ARG_ONE /* remove */,
ARG_ONE /* open */,       1 /* filesize */,
ARG_TWO /* read */,       ARG_TWO /*write */,
0       /* seek */,       0 /* tell */,
0       /* close */
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

static bool is_valid_uaddr (uint32_t *pd, const void *uaddr);

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
  int32_t args[3]; /* All args on stack are 4 bytes; cast as appropriate. */
  /* TODO look up in a table. */

  for(i = 0; i < syscall_nargs[syscall - SYS_MIN]; i++)
  {
    args[i] = *(int32_t *) stack_pop (&sp);
    int ptr_bit = (1 << i);
    if (syscall_isptr[syscall - SYS_MIN] & ptr_bit)
    {
      void *p = (void *) args[i];
      /* Verify that pointer points to valid user space. */
      if (!is_valid_uaddr (pd, p))
      {
        /* If invalid, cleanly terminate the caller. */
        syscall = SYS_EXIT;
        break;
      }
    }
  }

  /* Now all of the args have been tested for safety, so
    we just pass the buck. */
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

static void syscall_halt ()
{
  /* TODO Anything else? */
  shutdown_power_off ();
}

static void syscall_exit (int status)
{
  /* TODO */
  return;
}

static pid_t syscall_exec (const char *cmd_line)
{
  /* TODO */
  return 0;
}

static int syscall_wait (pid_t pid)
{
  /* TODO */
  return -1;
}

static bool syscall_create (const char *file, unsigned initial_size)
{
  /* TODO */
  return false;
}

static bool syscall_remove (const char *file)
{
  /* TODO */
  return false;
}

static int syscall_open (const char *file)
{
  /* TODO */
  return -1;
}

static int syscall_filesize (int fd)
{
  /* TODO */
  return -1;
}

static int syscall_read (int fd, void *buffer, unsigned size)
{
  /* TODO */
  return -1;
}

static int syscall_write (int fd, const void *buffer, unsigned size)
{
  ASSERT (STDOUT_FILENO <= fd);

  unsigned n_to_write;
  unsigned n_left = size;
  unsigned n_written = 0;

  if (fd == STDOUT_FILENO)
  {
    while (n_left)
    {
      /* Write no more than IO_BUFSIZE. */
      n_to_write = (IO_BUFSIZE <= n_left ? IO_BUFSIZE : n_left);
      putbuf (buffer + n_written, n_to_write);
      n_written += n_to_write;
      n_left -= n_to_write;
    }
  }
  else
  {
    /* TODO */
    n_written = -1;
  }

  return n_written;
}

static void syscall_seek (int fd, unsigned position)
{
  /* TODO */
  return;
}

static unsigned syscall_tell (int fd)
{
  /* TODO */
  return 0;
}

static void syscall_close (int fd)
{
  /* TODO */
  return;
}

/* Returns true if uaddr is non-null and is valid in this pagedir, and false else. */
static
bool is_valid_uaddr (uint32_t *pd, const void *uaddr)
{
  ASSERT (pd != NULL);

  return (uaddr != NULL && is_user_vaddr (uaddr) && pagedir_get_page (pd, uaddr) != NULL);
}
