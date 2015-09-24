/* Invokes an invalid system call. Should exit -1. 
 
   Test submitted by Jamie Davis <davisjam@vt.edu>, Fall 2015. */

#include <syscall-nr.h>
#include "tests/lib.h"
#include "tests/main.h"

void
test_main (void) 
{
  int syscall = 99999;

  /* Invoke the system call. */
  asm volatile ("movl %0, %%esp; int $0x30" : : "g" (&syscall));
  fail ("should have called exit(-1)");
}
