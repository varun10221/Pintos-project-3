#include "userprog/syscall.h"

#include <stdio.h>
#include <stdbool.h>
#include <syscall-nr.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
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
#include "filesys/directory.h"
#include "lib/user/syscall.h"
#include "vm/page.h"

/* Size of tables. */
#define N_SUPPORTED_SYSCALLS 20

enum io_type 
  {
    IO_TYPE_READ, /* IO will read. */
    IO_TYPE_WRITE /* IO will write. */
  };

/* For avoiding page faults when working with user-provided strings. */
static void * copy_str_into_sp (const char *str);

/* For testing user-provided addresses. */
static bool maybe_valid_uaddr (const void *uaddr);

static bool copy_from_user (void *dest, const void *src, unsigned n_bytes);
static bool copy_int32_t_from_user (int32_t *dest, const int32_t *src);

static bool pop_int32_t_from_user_stack (int32_t *dest, int32_t **stack);

static int get_user (const uint8_t *uaddr);

/* For converting a path to standard form */
static char * convert_string_to_std_form (const char *);

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
1 /* munmap */, 1 /* chdir */,
1 /* mkdir */,  2 /* readdir */,
1 /* isdir */,  1 /* inumber */
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
static bool syscall_chdir (const char *dir);
static bool syscall_mkdir (const char *dir);
static bool syscall_readdir (int fd, char *name);
static bool syscall_isdir (int fd);
static int syscall_inumber (int fd);

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

  /* TODO Same thinking as below, do we need to be this careful
     now or can we just make sure sp is below PHYS_BASE and
     let page_fault deal with it?  */

  thread_current ()->getting_syscall_args = true;
  /* Identify syscall. */
  int32_t syscall;
  if (!pop_int32_t_from_user_stack (&syscall, (int32_t **) &sp))
    syscall_exit (-1);

  /* Invalid syscall? */
  if (syscall < SYS_MIN || SYS_MAX < syscall)
    syscall_exit(-1);

  /* Extract the args. */
  int32_t args[3]; 
  for (i = 0; i < syscall_nargs[syscall - SYS_MIN]; i++)
  {
    if (!pop_int32_t_from_user_stack (&args[i], (int32_t **) &sp))
      syscall_exit (-1);
  }
  thread_current ()->getting_syscall_args = false;

  /* We made it this far safely, so update the min observed stack address. */
  process_observe_stack_address (sp);

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
      /* TODO Is this the correct return value for f->eax on a bool?*/
      f->eax = (syscall_create ((char *) args[0], (unsigned) args[1]) ? 1 : 0);
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

    /* Project 4 only. */
    case SYS_CHDIR:                  /* Change the current directory. */
      /* TODO Is this the correct return value for f->eax on a bool?*/
      f->eax = (syscall_chdir ((const char *) args[0]) ? 1 : 0);
      break;
    case SYS_MKDIR:                  /* Create a directory. */
      /* TODO Is this the correct return value for f->eax on a bool?*/
      f->eax = (syscall_mkdir ((const char *) args[0]) ? 1 : 0);
      break;
    case SYS_READDIR:                /* Reads a directory entry. */
      /* TODO Is this the correct return value for f->eax on a bool?*/
      f->eax = (syscall_readdir ((int) args[0], (char *) args[1]) ? 1 : 0);
      break;
    case SYS_ISDIR:                  /* Tests if a fd represents a directory. */
      /* TODO Is this the correct return value for f->eax on a bool?*/
      f->eax = (syscall_isdir ((int) args[0]) ? 1 : 0);
      break;
    case SYS_INUMBER:                 /* Returns the inode number for a fd. */
      f->eax = syscall_inumber ((int) args[0]); 
      break;

    /* Un-implemented syscalls: drop through to the printf. */
    default:
      printf ("Unsupported system call! vec %i esp %i\n", f->vec_no, *(int32_t *) f->esp);
  };  
}

/* Implementations of syscalls. */

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
  if (file == NULL)
    /* TODO Argue with Dr. Back about the create-null test. Per P2 specs, returning false should suffice. */ 
    syscall_exit (-1);
    /* return false; */

  /* Copy name into process's scratch page so that it will be safe from page fault. */
  char *sp = copy_str_into_sp (file);
  if (!sp)
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
  if (file == NULL)
    return -1;

  /* Copy name into process's scratch page so that it will be safe from page fault. */
  char *sp = copy_str_into_sp (file);
  if (!sp)
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
  if (file == NULL)
    return -1;

  /* Copy name into process's scratch page so that it will be safe from page fault. */
  char *sp = copy_str_into_sp (file);
  if (!sp)
    return false;

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
  struct file *f = (struct file *) process_fd_lookup (fd, FD_FILE);
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

  struct file *f = NULL;
  if (fd != STDOUT_FILENO)
  {
    /* Get the corresponding file. */
    f = (struct file *) process_fd_lookup (fd, FD_FILE);

    if (f == NULL)
      /* We don't have this fd open. */
      return -1;
  }

  /* NB If doing file I/O, this implementation allows "races" with other readers/writers. 
     File content can vary based on IO interleaving.
     See Piazza, but basically the FS makes no atomicity guarantees in the presence of races. */
  while (n_left)
  {
    int amount_to_read = PGSIZE;
    /* If buffer is not page-aligned, only read up to the end of the page so that 
       subsequent non-final reads will be page-aligned. */
    uint32_t mod_PGSIZE = (uint32_t) buffer % PGSIZE;
    if (mod_PGSIZE != 0)
      /* Read from buffer to the end of its containing page. */
      amount_to_read = PGSIZE - mod_PGSIZE;
    /* n_left is the upper bound on the amount to read. */
    if (n_left < amount_to_read)
      amount_to_read = n_left;

    /* For each page spanned by buffer, pin the page to a frame so that we cannot
       page fault while we are holding filesys_lock(). */
    struct page *pg = process_page_table_find_page (buffer);
    /* Invalid mem access? Bail out. */
    if (pg == NULL)
      syscall_exit (-1);

    process_pin_page (pg);

    /* Attempt to modify buf -- if we're reading into a buffer in a read-only page, page_fault
       will error out here safely before we hold filesys_lock(). 
       This code is not necessary in syscall_write because there the buffer is only being read from. 

       The magic is to defeat compiler "optimization", a la http://stackoverflow.com/questions/2219829/how-to-prevent-gcc-optimizing-some-statements-in-c */
    ((char volatile *)buffer)[0] = ((char *)buffer)[0];

    int32_t amount_read = -1;
    if (fd == STDOUT_FILENO)
    {
      /* This page of the buffer is pinned, so read amount_to_read chars. */
      char *buf = (char *) buffer;
      int i;
      for (i = 0; i < amount_to_read; i++)
        buf[i] = (char) input_getc ();
      amount_read = amount_to_read;
    }
    else
    {
      filesys_lock ();
      amount_read = file_read (f, buffer, amount_to_read);
      filesys_unlock ();
    }

    process_unpin_page (pg);
  
    /* Advance buffer. */
    buffer += amount_read;

    /* Update counters. */
    n_read += amount_read;
    n_left -= amount_read;
    /* File I/O only: if we read a non-negative value but less than the expected amount, we've reached end of file. */
    ASSERT (0 <= amount_read && amount_read <= amount_to_read);
    if (amount_read != amount_to_read)
      break;
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

  int n_left = (int) size;
  int n_written = 0;

  struct file *f = NULL;
  if (fd != STDOUT_FILENO)
  {
    /* Get the corresponding file. */
    f = (struct file *) process_fd_lookup (fd, FD_FILE);
    if (f == NULL)
      /* We don't have this fd open. */
      return -1;
  }

  /* NB If doing file I/O, this implementation allows "races" with other readers/writers. 
     File content can vary based on IO interleaving.
     See Piazza, but basically the FS makes no atomicity guarantees in the presence of races. */
  while (n_left)
  {
    int amount_to_write = PGSIZE;
    /* If buffer is not page-aligned, only write up to the end of the page so that 
       subsequent non-final writes will be page-aligned. */
    uint32_t mod_PGSIZE = (uint32_t) buffer % PGSIZE;
    if (mod_PGSIZE != 0)
      /* Write from buffer to the end of its containing page. */
      amount_to_write = PGSIZE - mod_PGSIZE;
    /* n_left is the upper bound on the amount to write. */
    if (n_left < amount_to_write)
      amount_to_write = n_left;

    /* For each page in buffer, pin the page to a frame so that we cannot
       page fault while we are holding filesys_lock(). */
    struct page *pg = process_page_table_find_page (buffer);
    /* Invalid mem access? Bail out. */
    if (pg == NULL)
      syscall_exit (-1);

    process_pin_page (pg);

    int32_t amount_written = -1;
    if (fd == STDOUT_FILENO)
    {
      /* TODO This was incorrect. No test did a good job of catching it. A test that calls printf on a giant string (multiple pages) would be useful.
      putbuf (buffer + n_written, amount_to_write); */
      putbuf (buffer, amount_to_write);
      amount_written = amount_to_write;
    }
    else
    {
      filesys_lock ();
      amount_written = file_write (f, buffer, amount_to_write);
      filesys_unlock ();
    }

    process_unpin_page (pg);
  
    /* Advance buffer. */
    buffer += amount_written;

    /* Update counters. */
    n_written += amount_written;
    n_left -= amount_written;
    /* File I/O only: if we write a non-negative value but less than the expected amount, we've reached end of file. */
    ASSERT (0 <= amount_written && amount_written <= amount_to_write);
    if (amount_written != amount_to_write)
      break;
  }

  return n_written;
}

/* Changes the next byte to be read or written in open file fd to position, 
   expressed in bytes from the beginning of the file. */
static void 
syscall_seek (int fd, unsigned position)
{
  filesys_lock ();
  struct file *f = (struct file *) process_fd_lookup (fd, FD_FILE);
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
  struct file *f = (struct file *) process_fd_lookup (fd, FD_FILE);
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
static mapid_t 
syscall_mmap (int fd, void *addr)
{
  struct file *f = NULL;
  struct mmap_info *mmap_info = NULL;
  mapid_t mapping = -1;

  f = (struct file *) process_fd_lookup (fd, FD_FILE);
  if (f == NULL)
    goto CLEANUP_AND_ERROR;

  /* Populate an mmap_details struct. */
  struct mmap_details md;
  memset (&md, 0, sizeof(struct mmap_details));
  md.mmap_file = f;
  md.offset = 0;
  md.backing_type = MMAP_BACKING_PERMANENT;
  mmap_info = process_add_mapping (&md, addr, MAP_PRIVATE|MAP_RDWR);
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
static void 
syscall_munmap (mapid_t mapping)
{
  struct mmap_info *mmap_info = process_mmap_lookup (mapping);

  /* No such mapping. */
  if (mmap_info == NULL)
    return;

  process_delete_mapping (mmap_info);
  process_mmap_remove (mapping);
}

/* Changes the current working directory of the process to dir, which may be relative or absolute. 
   Returns true if successful, false on failure. */
static bool 
syscall_chdir (const char *dir)
{
  if (dir == NULL)
    syscall_exit (-1);
#if 0 
  char *temp_path;
  temp_path = malloc ((strlen (dir) + 1) * sizeof (char));
  temp_path = convert_string_to_std_form (dir);
#endif
 
  bool success;
  /* Copy name into process's scratch page so that it will be safe from page fault. */
  char *sp = copy_str_into_sp (dir);
  if (!sp)
    return false;

  success = process_chdir (sp);
  return success;
  
}

/* Creates the directory named dir, which may be relative or absolute. 
   Returns true if successful, false on failure. Fails if dir already exists or 
     if any directory name in dir, besides the last, does not already exist. */
static bool 
syscall_mkdir (const char *dir)
{
  if (dir == NULL)
    syscall_exit (-1);

  /* Copy name into process's scratch page so that it will be safe from page fault. */
  char *sp = copy_str_into_sp (dir);
  if (!sp)
    return false;
  return filesys_create_dir (sp);
}

/* Reads a directory entry from file descriptor fd, which must represent a directory. 
   If successful, stores the null-terminated file name in name, which must have room 
     for READDIR_MAX_LEN + 1 bytes, and returns true. 
   If no entries are left in the directory, returns false.  
 
   "." and ".." should not be returned by readdir. */
static bool 
syscall_readdir (int fd, char *name)
{
  char *buf = process_scratch_page_get  ();
  if (!buf)
    return false;
  struct dir *d = (struct dir *) process_fd_lookup (fd, FD_DIRECTORY);
  if (d)
  {
    bool success = false;
    do
    {
      memset (buf, 0, READDIR_MAX_LEN + 1); /* Don't leak previous file names. */
      success = dir_readdir (d, buf);
    } while (strcmp (".", buf) == 0 || strcmp ("..", buf) == 0); /* Skip . and .. */

    if (success)
      /* Copy the latest result from BUF into NAME. */
      strlcpy (name, buf, MIN (READDIR_MAX_LEN + 1, PGSIZE));
    return success;
  }
  else
    return false;
}

/* Returns true if fd represents a directory, 
   false if it represents an ordinary file. */
static bool 
syscall_isdir (int fd)
{
  struct dir *d = (struct dir *) process_fd_lookup (fd, FD_DIRECTORY);
  if (d)
    return true;
  return false;
}

/* Returns the inode number of the inode associated with fd, 
   which may represent an ordinary file or a directory. 

   Returns -1 if no such fd. */
static int 
syscall_inumber (int fd)
{
  return process_fd_inumber (fd);
}

/* Helper functions. */

/* Copy STR into the process's stack page.
   STR must not be NULL, though it can be an empty string. 

   Returns a pointer to the copy in the stack page. The copy can
     be accessed without the risk of a page fault.
   If STR is too long to fit into the stack page (PGSIZE bytes),
     returns NULL. */ 
static void *
copy_str_into_sp (const char *str)
{
  ASSERT (str != NULL);
  ASSERT (PATH_MAX <= PGSIZE);
  char *sp = (char *) process_scratch_page_get ();
  ASSERT (sp != NULL);

  size_t len = strlcpy (sp, str, PGSIZE);
  /* If too long, fail gracefully (a la ENAMETOOLONG). */
  if (PATH_MAX < len+1) 
    return NULL;
  return sp;
}


/* Returns true if UADDR is non-null and below kernel space. 
   UADDR may point to un-mapped user space. */
static bool 
maybe_valid_uaddr (const void *uaddr)
{
  return (uaddr != NULL && is_user_vaddr (uaddr));
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
    stack_pop ((void **) stack);
  return success;
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


/* TODO (not called by any function temporarily) 
   Convert the path to a standard form by replacing 
   multiple '/' (if any)to single */  
static char *
convert_string_to_std_form (const char *path)
{
  int length = strlen (path) + 1;
  char *a;
  a = (char*) malloc (length * sizeof (char));
  int i = 0;
  char previous_char;
  while (i <= length )
    {
        if ( i == 0)
         { a[i] = path[i];
           previous_char = a[i];
         }
        
        if (previous_char == '/' && path [i] =='/' )
           {
             /* Need to refactor code in a better way */
           } 
        else
           {
             a[i] = path[i];
             previous_char = a[i];
           }
        i++;
    }
  /* check if the final char in a is null terminator , else add one */
  if(a[i] !='\0')
    a[i] = '\0';
  return a; 
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
