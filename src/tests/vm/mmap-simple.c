/* Simple mmap test: mmap several files and close them in reverse order.
   Does no reads or writes of the mmap'd regions.
   Just a sanity test that the system calls don't blow up.

   Test submitted by Jamie Davis <davisjam@vt.edu>, Fall 2015. */

#include <stdio.h>
#include <syscall.h>
#include "tests/lib.h"
#include "tests/main.h"

#include <round.h>

#define INIT_MAPPING ((void *) 0x10000000)
#define PGSIZE 4096

int n_mappings = 10;

void
test_main (void)
{
  int handle;
  int i;
  mapid_t maps[n_mappings];
  void *addrs[n_mappings];

  CHECK ((handle = open ("sample.txt")) > 1, "open \"sample.txt\"");

  addrs[0] = INIT_MAPPING;
  for (i = 1; i < n_mappings; i++)
    addrs[i] = (void *) ROUND_UP ((size_t) addrs[i-1] + filesize (handle), PGSIZE);

  /* mmap. */
  for (i = 0; i < n_mappings; i++)
  {
    printf ("mmap (%i, %p)\n", handle, addrs[i]);
    CHECK ((maps[i] = mmap (handle, addrs[i])) != MAP_FAILED, "mmap \"sample.txt\"");
  }

  /* munmap, in reverse order. */
  for (i = n_mappings; 0 <= i; i--)
    munmap (maps[i]);

  close (handle);
}
