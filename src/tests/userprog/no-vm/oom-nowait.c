/* Verify that no memory is leaked regardless of the exit
   order of parents and children, and regardless of whether or
   not the parent waits for its children.
 
   Grandparent: 
     for(1..10):
       Launch a "launcher" child that launches as many children as possible,
       and exits without waiting on any of them.
       Each iteration this launcher decides whether to exit BEFORE
       or AFTER its children do.

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

   To have the children exit before the parent, the parent uses a
   file to communicate: either creation of a signal file, or making
   a signal file writable. Either way should work fine.

   TODO I am observing extreme contention (for filesys_lock?) once the child count
   reaches 20. I'm guessing that file_open holds the filesys_lock for longer.
   Consequently I anticipate that introducing YET ANOTHER process that is the
   only one trying file_open, and having the children wait_for_process_to_finish
   on that signal PROCESS, might be a good route. 
   I have other work to do, so I will leave that as "future work". For now,
   LAUNCHER_EXITS_FIRST mode will be disabled.

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

/* Used in CHILDREN_EXIT_FIRST mode to signal the children to exit. */
const char *signal_file = "oom-nw-signal";

enum process_type { GRANDPARENT, LAUNCHER, CHILD };
enum behavior 
{ 
  BEHAVIOR_MIN,
  LAUNCHER_EXITS_FIRST = BEHAVIOR_MIN, 
  CHILDREN_EXIT_FIRST, 
  BEHAVIOR_MAX = CHILDREN_EXIT_FIRST
};

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

/* WATCH_FILE: The executable of an actively-running process
   Wait until that process has "probably" exited.

   The contents of WATCH_FILE will be modified by this routine. */
static void
wait_for_process_to_finish (const char *watch_file)
{
  ASSERT (watch_file != NULL);
  /* Open and try to write to it. */
  int fd = open (watch_file);
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

/* WATCH_FILE: A file that is created to signal that
   we should return. We return after the file exists
   and we have waited long enough that the creator
   has probably had time to exit if he does so promptly.

   The contents of WATCH_FILE will be modified by this routine. */
static void
wait_for_file_to_exist (const char *watch_file)
{
  ASSERT (watch_file != NULL);

  /* DEBUGGING */
  int spin_counter = 100000 + random_ulong () % 5000;

  /* Wait until open succeeds. */
  int fd = -1;
  do{
    fd = open (watch_file);
    /* Don't spin on the mutex for the filesys in the kernel. 
       This should be long enough to consume a TIME_SLICE and
       force preemption. */
    int result = wait_a_bit(spin_counter);
  } while (fd < 0);

  /* Open succeeded, so make a few writes. This should give
     the parent process plenty of time to finish exiting. */
  int i;
  char c = 'Z';
  for (i = 0; i < 5; i++)
  {
    ASSERT (write (fd, &c, 1) == 1);
    int result = wait_a_bit(spin_counter);
  }

  close (fd);
}

/* For launcher: spawn children, exit before or after they do based on BEHAV
   Return the number of children we created. */ 
static int 
launcher_run (enum behavior behav, const char *launcher_executable, const char *child_executable)
{
  ASSERT (launcher_executable != NULL);
  ASSERT (child_executable != NULL);

  /* Fresh copy of our executable for the children to use. */
  copy_file (child_executable, launcher_executable);

  /* Should be plenty... */
  int MAX_CHILDREN = 512;

  if (behav == CHILDREN_EXIT_FIRST)
    remove (signal_file);

  /* Spawn as many children as we can. */
  int n_children = 0;
  do{
    /* Launch a child. */
    /* PROCESS_TYPE BEHAVIOR CHILD_ID WATCH_FILE */
    char child_cmd[128];
    const char *watch_file = (behav == LAUNCHER_EXITS_FIRST ? launcher_executable : signal_file);
    snprintf (child_cmd, sizeof child_cmd,
              "%s %i %i %i %s", 
              child_executable, CHILD, behav, n_children, watch_file);
    pid_t child_pid = exec (child_cmd);

    /* Can't create any more children. */
    if (child_pid == -1)
      break;

    n_children++;
    /* Paranoia: make sure children are actually waiting so that we run out of memory. */
    ASSERT (n_children <= MAX_CHILDREN);
  } while(1);

  /* If waiting for children, signal them and then wait on the executable. */
  if (behav == CHILDREN_EXIT_FIRST)
  {
    ASSERT (create (signal_file, 256));
    wait_for_process_to_finish (child_executable);
  }

  return n_children;
}

/* For child: wait on WATCH_FILE in the fashion indicated by BEHAV. */
static void
child_run (enum behavior behav, const char *watch_file)
{
  ASSERT (watch_file != NULL);
  if (behav == LAUNCHER_EXITS_FIRST)
    wait_for_process_to_finish (watch_file);
  else
    wait_for_file_to_exist (watch_file);

  return;
}

/* The first copy is invoked without command line arguments.
   subsequent copies are invoked with: 
   launcher: PROCESS_TYPE BEHAVIOR
   children: PROCESS_TYPE BEHAVIOR CHILD_ID WATCH_FILE */
int
main (int argc, char *argv[])
{
  int process_type; 
  enum behavior behav;
  int child_id;
  char *watch_file; 

  bool is_grandparent = (argc == 1);
  if (is_grandparent)
    process_type = GRANDPARENT;
  else
  {
    process_type = atoi(argv[1]);
    behav = atoi(argv[2]);
    if (process_type == CHILD)
    {
      child_id = atoi(argv[3]);
      watch_file = argv[4];
    }
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
      behav = BEHAVIOR_MIN + random_ulong () % (BEHAVIOR_MAX - BEHAVIOR_MIN + 1);

      /* TODO Make it so that CHILDREN_EXIT_FIRST is feasible. See comments at the top.*/
      behav = LAUNCHER_EXITS_FIRST;

      char launcher_cmd[128];
      /* launcher: PROCESS_TYPE BEHAVIOR */
      snprintf (launcher_cmd, sizeof launcher_cmd,
                "%s %i %i", launcher_executable, LAUNCHER, behav);

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
    return launcher_run (behav, launcher_executable, child_executable);
  }
  else if (process_type == CHILD)
  {
    random_init (child_id);
    child_run (behav, watch_file);
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
