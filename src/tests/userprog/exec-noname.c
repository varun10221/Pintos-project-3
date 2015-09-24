/* Executes an empty string. 

   Test submitted by Jamie Davis <davisjam@vt.edu>, Fall 2015. */

#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

void
test_main (void) 
{
  int child = exec ("");
  int rc = wait (child);
  if (child != -1 || rc != -1)
    fail("Error, exec(\"\") returned %i, wait returned %i. Expected -1, -1\n", child, rc);
}
