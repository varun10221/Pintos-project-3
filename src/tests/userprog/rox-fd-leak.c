/* Ensure that the executable of a running process cannot be
   modified. 
   
   This test checks for leaks in the file descriptor table.
   For example, can the test close the file descriptor that
   keeps its own executable non-writable?

   Test submitted by Jamie Davis <davisjam@vt.edu>, Fall 2015. */

#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

static const int MIN_FD = -4097;
static const int MAX_FD = 4098;

void
test_main (void) 
{
  int handle;
  char buffer[16];

  int i;
  for (i = MIN_FD; i < MAX_FD; i++)
  {
    /* Skip stdout, stderr. */
    if (i == 0 || i == 1) continue;
    close (i);
  }
  
  CHECK ((handle = open ("rox-fd-leak")) > 1, "open \"rox-fd-leak\"");
  CHECK (read (handle, buffer, sizeof buffer) == (int) sizeof buffer,
         "read \"rox-fd-leak\"");
  CHECK (write (handle, buffer, sizeof buffer) == 0,
         "try to write \"rox-fd-leak\"");
}
