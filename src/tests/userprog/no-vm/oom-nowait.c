/* Verify that no memory is leaked regardless of the exit
   order of parents and children, and regardless of whether or
   not the parent waits for its children.
 
   Grandparent: 
     for(1..10):
       Launch a child that launches as many children as possible,
       and exits without waiting on any of them (BEFORE any finish)

   The grandparent ensures that the same number of children can be 
   launched each time.

   This leaves us with the following problem: how can the child know 
   whether or not the parent has finished (so that it does not exit
   before its parent does)? 

   With the current set of syscalls, we cannot be perfect at this.
   However, with sufficient children, we can be confident that the desired exit order
   will be accomplished "enough times" that over NUM_REPETITIONS iterations
   we'll encounter it often enough to detect a memory leak.
   
   The technique is this: Per 3.3.5, we know that processes lock their executable 
   until they are done using it (i.e. until they are about to exit).

   A process A can determine whether or not another process B is almost done 
   by writing to B's executable until the write succeeds. 
   Now A knows that B is "finishing up", and provided it spends some additional
   time doing a relatively expensive operation, the probability that B has exited
   is good. The more expensive the operation, the more likely it is that B has
   exited.

   Test submitted by Jamie Davis <davisjam@vt.edu>, Fall 2015. */

#include <debug.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <syscall.h>
#include <random.h>
#include "tests/lib.h"

static const int NUM_REPETITIONS = 10;
static const int MIN_CHILDREN = 20;

const char *test_name = "oom-nowait";

/* These executables are created each iteration. */
const char *launcher_executable = "oom-nw-launch";
const char *child_executable = "oom-nw-child";

enum process_type { GRANDPARENT, LAUNCHER, CHILD };

/* Copy src to dst. */
static void
copy_file (const char *dst, const char *src)
{
  int src_fd = open (src);
  ASSERT (0 <= src_fd);
  int src_fsize = filesize (src_fd);

  /* Delete, create, open the dst file. */
  remove (dst);
  bool create_worked = create (dst, src_fsize);
  ASSERT (create_worked);
  int dst_fd = open (dst);
  ASSERT (0 <= dst_fd);

  char buf[512];
  int blocksize = 512;
  int nblocks = src_fsize / blocksize;
  int last_round = src_fsize % blocksize;

  int i;
  for (i = 0; i < nblocks; i++)
  {
    int n_read = read (src_fd, buf, blocksize);
    ASSERT (n_read == blocksize);
    int n_written = write (dst_fd, buf, blocksize);
    ASSERT (n_written == blocksize);
  }
  if (last_round)
  {
    int n_read = read (src_fd, buf, last_round);
    ASSERT (n_read == last_round);
    int n_written = write (dst_fd, buf, last_round);
    ASSERT (n_written == last_round);
  }

  close (src_fd);
  close (dst_fd);
}

/* Modify a pointer so we don't get optimized away. 
   Basically a brief sleep. */
static int
wait_a_bit (int n_spins)
{
  int i;
  int count = 0;
  for(i = 0; i < n_spins; i++)
    count++; 
  return count;
}

/* EXECUTABLE_TO_WATCH: The executable of an actively-running process
   Wait until that process has "probably" exited.
   The executable will be modified by this routine. */
static void
wait_for_process_to_finish (const char *executable_to_watch)
{
  ASSERT (executable_to_watch != NULL);
  /* Open and try to write to it. */
  int fd = open (executable_to_watch);
  ASSERT (0 <= fd);

  int spin_counter = 10000;

  char c = 'Z';
  int rc = 0;
  /* Wait until write succeeds. */
  do{
    /* Don't spin on the mutex for the filesys in the kernel. 
       This should be long enough to consume a TIME_SLICE and
       force preemption. */
    int result = wait_a_bit(spin_counter);

    rc = write (fd, &c, 1);
  } while (rc != 1);

  /* Write succeeded, so make a few more writes. This should give
     the parent process plenty of time to finish exiting. */
  int i;
  for (i = 0; i < 5; i++)
    rc = write (fd, &c, 1);

  close (fd);
}

/* For launcher: spawn children, exit before waiting for them.
   Returns the number of children we created. */ 
static int 
launcher_run (const char *launcher_executable, const char *child_executable)
{
  ASSERT (launcher_executable != NULL);
  ASSERT (child_executable != NULL);

  /* Fresh copy of our executable for the children to use. */
  copy_file (child_executable, launcher_executable);

  /* Should be plenty... */
  int MAX_CHILDREN = 512;

  /* Spawn as many children as we can. */
  int n_children = 0;
  do{
    /* Launch a child. */
    /* PROCESS_TYPE EXECUTABLE_TO_WATCH */
    char child_cmd[128];
    snprintf (child_cmd, sizeof child_cmd,
              "%s %i %s", 
              child_executable, CHILD, launcher_executable);
    pid_t child_pid = exec (child_cmd);
    /* Can't create any more children. */
    if (child_pid == -1)
      break;

    n_children++;
    /* Paranoia: make sure children are actually waiting so that we run out of memory. */
    ASSERT (n_children <= MAX_CHILDREN);
  } while(1);

  /* Return before wait'ing. */
  return n_children;
}

/* For child: if we wait, wait on executable_to_watch. */
static void
child_run (const char *executable_to_watch)
{
  ASSERT (executable_to_watch != NULL);
  wait_for_process_to_finish (executable_to_watch);
  return;
}

/* The first copy is invoked without command line arguments.
   subsequent copies are invoked with: 
   launcher: PROCESS_TYPE
   children: PROCESS_TYPE EXECUTABLE_TO_WATCH */
int
main (int argc, char *argv[])
{
  int process_type; 
  char *executable_to_watch; 

  bool is_grandparent = (argc == 1);
  if (is_grandparent)
    process_type = GRANDPARENT;
  else
  {
    process_type = atoi(argv[1]);
    if (process_type == CHILD)
      executable_to_watch = argv[2];
  }

  /* Internal error, debugging code. */
  if(process_type != GRANDPARENT && process_type != LAUNCHER && process_type != CHILD)
    fail("ERROR, HOW DID I GET process_type %i??\n", process_type); 

  /* Number of children spawned on the first iteration. */
  int orig_n_children = -1;
  if (process_type == GRANDPARENT)
  {
    msg ("begin");
    random_init (0);

    int i;
    for (i = 0; i < NUM_REPETITIONS; i++)
    {
      /* Fresh copy of our executable for the launcher. */
      copy_file (launcher_executable, test_name);

      char launcher_cmd[128];
      snprintf (launcher_cmd, sizeof launcher_cmd,
                "%s %i", launcher_executable, LAUNCHER);

      pid_t launcher_pid = exec (launcher_cmd);
      int n_children = wait (launcher_pid);
      
      /* First time through, record the number. */
      if (i == 0)
      {
        ASSERT (MIN_CHILDREN <= n_children);
        orig_n_children = n_children;
      }
      /* Subsequent iterations, verify we got a matching number of children. */
      else
      {
        if (n_children != orig_n_children)
          fail ("Error, on iteration %i I spawned %i children != orig %i\n", i, n_children, orig_n_children);
      }

      /* Wait until the children are done.
         This needs to be a certainty or else we give false errors, so
         wait repeatedly. Pathological scheduling may still break us. */
        int j;
        for(j = 0; j < 5; j++)
          wait_for_process_to_finish (child_executable);

    } /* Loop for NUM_REPETITIONS. */
  } /* GRANDPARENT. */
  else if (process_type == LAUNCHER)
  {
    return launcher_run (launcher_executable, child_executable);
  }
  else if (process_type == CHILD)
  {
    child_run (executable_to_watch);
    return 0;
  }
  else
    NOT_REACHED ();

  /* Clean up. */
  remove (launcher_executable);
  remove (child_executable);

  msg ("success. program created at least %i children each iteration, and the same number were created each time.", MIN_CHILDREN);
  msg ("end");

  return orig_n_children;
}
