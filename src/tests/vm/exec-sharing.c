/* Test for executable memory sharing. 
 
  Parent creates N_CHILDREN children, all running the same
  executable.

  Children wait until parent writes a byte to done_file, at which point
  they all exit. This will encourage concurrent exiting, which
  will exercise whatever ``un-share'' logic is used to support
  sharing.

  TODO Just have parent launch children until it gets a failure.
 
  Test submitted by Jamie Davis <davisjam@vt.edu>, Fall 2015. */

#include <debug.h>
#include <syscall.h>
#include <stdlib.h>
#include "tests/lib.h"
#include "tests/main.h"

char *done_file = "parent_done";
void child_run (char *done_file);
void parent_run (char *exec_file, char *done_file, int n_children);

void child_run (char *done_file)
{
  ASSERT (done_file != NULL);

  int fd = open (done_file);
  if (fd < 0)
    fail ("child_run: Error, open (%s) gave %i\n", done_file, fd);

  /* Loop until first int of file is non-zero. */
  int done = 0;
  while (1)
  {
    seek (fd, 0);
    read (fd, &done, sizeof(done));
    if (done)
      break;
    else
    {
      /* Spin. */
      int max = 10000;
      int i, counter;
      for (i = 0; i < max; i++)
        counter++;
    }
  }
  close (fd);
}

void parent_run (char *exec_file, char *done_file, int n_children)
{
  ASSERT (exec_file != NULL);
  ASSERT (done_file != NULL);

  remove (done_file); /* Clean up. */

  bool success = create (done_file, 1);
  if (!success)
    fail ("parent_run: Could not create done_file %s\n", done_file);

  int fd = open (done_file);
  if (fd < 0)
    fail ("parent_run: open (%s) gave %i\n", done_file, fd);

  int done = 0;
  seek (fd, 0);
  write (fd, &done, sizeof(done));

  /* Start children. */
  int i;
  for (i = 0; i < n_children; i++)
    exec (exec_file);
  /* Update done_file. */
  done = 1;
  seek (fd, 0);
  write (fd, &done, sizeof(done));
}

int
main (int argc, char *argv[]) 
{
  char *exec_name = NULL;
  int n_children = -1;
  if (argc == 2)
  {
    exec_name = argv[0];
    n_children = atoi (argv[1]);
    parent_run (exec_name, done_file, n_children);
  }
  else if (argc == 1)
    child_run (done_file);
  else
    return -1;

  return 0;
}
