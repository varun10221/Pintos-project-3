/* Tries to open the same file N_TIMES_TO_OPEN times, repeating N_REPETITIONS iterations.
   Must succeed and must return a different file descriptor
   in each iteration. 
   
   if (CLOSE_FILES)
     We close all the fd's we open. Suitable if the submission has a maximum
     number of concurrent open fds.
   else
     We leak file descriptors each iteration, resulting in N_TIMES_TO_OPEN * N_REPETITIONS
     files open simultaneously.

   Test submitted by Jamie Davis <davisjam@vt.edu>, Fall 2015. */

#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

static const int N_TIMES_TO_OPEN = 100;
static const int N_REPETITIONS = 100;

static const bool CLOSE_FILES = true;

/* fail()'s if two fds in this set are identical or if any are < 0. */
static void
assert_fds_valid (int fds[], int n_fds)
{
  int i, j;
  for (i = 0; i < n_fds; i++)
  {
    int fd = fds[i];
    if (fd < 0)
      fail ("Error, found fd %i\n", fd);
    /* Between 0 and fd, there should be no duplicates. */
    for (j = 0; j < i; j++)
    {
      int comp_fd = fds[j];
      if (fd == comp_fd)
        fail ("Error, found fd %i at indices %i, %i within a single iteration\n", fd, i, j);
    }
  }
}

void
test_main (void) 
{
  int fds[N_TIMES_TO_OPEN];

  int i, j;
  for (i = 0; i < N_REPETITIONS; i++)
  {
    for (j = 0; j < N_TIMES_TO_OPEN; j++)
    {
      fds[j] = open ("sample.txt");
      if (fds[j] < 0)
        fail ("Error, open(\"sample.txt\") returned %i. repetition %i, open %i\n", fds[j], i, j);
    }
    assert_fds_valid (fds, N_TIMES_TO_OPEN);

    if (CLOSE_FILES)
    {
      for (j = 0; j < N_TIMES_TO_OPEN; j++)
        close (fds[j]);
    }

  } /* N_REPETITIONS loop */

}
