/* Test for very large files. 
   We fill the file system with a single enormous file.
   We write until write() fails, and ensure that the failure
     occurs above a minimal maximum file size.
   We create, write, and delete this file. 
   On subsequent iterations we ensure that the same maximum file
     size is obtained.
   
   Half the time we ensure that contents from previous versions of 
     the file are not "leaked" by making a sparse file and reading 
     prior to every write.
   Half the time we read after each write to verify contents.

  Test submitted by Jamie Davis <davisjam@vt.edu>, Fall 2015. */

#include <debug.h>
#include <syscall.h>
#include <stdlib.h>
#include <random.h>
#include <string.h>
#include <stdio.h>

#include "tests/lib.h"
#include "tests/main.h"

const char *test_name = "huge-file";
char *file = "disk-eater";
#define CHUNK_SIZE 12345

void print_buf (char *, int);

/* Prints BUF of size N_BYTES to stdout. */
void
print_buf (char *buf, int n_bytes)
{
  int i;
  for (i = 0; i < n_bytes; i++)
    printf ("%c", buf[i]);
}

int
main (int argc, char *argv[]) 
{
  if (argc != 3)
    fail ("huge-file: Error, usage: %s min_max_size_in_bytes niter", argv[0]);
  /* The minimum "max file size" we tolerate. */
  long min_max_size = atol (argv[1]);
  int niter = atoi (argv[2]);
  int i;

  long initial_max_file_size = -1;

  char zeros[CHUNK_SIZE];
  char out_buf[CHUNK_SIZE];
  char in_buf[CHUNK_SIZE];
  char tmp_buf[CHUNK_SIZE];

  memset (zeros, 0, CHUNK_SIZE);
  random_bytes (out_buf, CHUNK_SIZE);

  for (i = 0; i < niter; i++)
  {
    int create_big = random_ulong () % 2;
    int sparse = random_ulong () % 2;
    //msg ("iter %i: create_big %i sparse %i", i, create_big, sparse);

    long bytes_written = 0;

    long init_size = (create_big ? min_max_size : 0);
    if (!create (file, init_size))
      fail ("Error, could not create (\"%s\", %li)", file, init_size);

    int fd = open (file);
    if (fd < 0)
      fail ("Error, open (%s, 0) gave fd %i\n", file, fd);

    if (sparse)
    {
      /* Make the file sparse and read prior to each write. */
      seek (fd, min_max_size);
      char c = 0;
      if (write (fd, &c, 1) != 1)
        fail ("Error, could not write file sparsely: offset %li\n", min_max_size);
      /* Now do a read-write sequence. Ensure every read returns nulls. */
      long offset = 0;
      while (1)
      {
        //msg ("read-write sequence: num %i offset %li", offset/CHUNK_SIZE, offset);
        /* Read and verify it's nulls. */
        seek (fd, offset); 
        long n_read = read (fd, in_buf, CHUNK_SIZE);
        if (n_read != CHUNK_SIZE && offset+CHUNK_SIZE < min_max_size)
          fail ("Error, read %li instead of %i and I'm not at the end of the file\n", n_read, CHUNK_SIZE);
        if (memcmp (in_buf, zeros, n_read))
        {
          printf ("Error, read something besides zeros between offset %li and %li\n", offset, offset + n_read);
          printf ("I expected:\n");
          print_buf (zeros, n_read);
          printf ("I read:\n");
          print_buf (in_buf, n_read);
          fail ("Error, see above.\n");
        }


        /* Write data. */
        seek (fd, offset); 
        long n_written = write (fd, out_buf, CHUNK_SIZE);
        bytes_written += n_written;

        int verify_after_write = random_ulong () % 2;
        if (verify_after_write)
        {
          seek (fd, offset);
          long n_read = read (fd, tmp_buf, CHUNK_SIZE);
          if (n_read != n_written)
            fail ("Error, verifying after write, and I read %li bytes after writing %li bytes\n", n_read, n_written);
          long j;
          for (j = 0; j < n_read; j++)
          {
            if (tmp_buf[j] != out_buf[j])
              fail ("Error, mismatch between write and subsequent read. Offset %li expected <%c> read <%c>\n", offset + j, out_buf[j], tmp_buf[j]);
          }
        }

        if (n_written != CHUNK_SIZE)
          break;

        offset += CHUNK_SIZE;
      }
    }
    else
    {
      /* Just write out the file until it fails. */
      seek (fd, 0);

      long offset = 0;
      while (1)
      {
        //msg ("write sequence: num %li offset %li", offset/CHUNK_SIZE, offset);
        long n_written = write (fd, out_buf, CHUNK_SIZE); 
        bytes_written += n_written;
        if (n_written != CHUNK_SIZE)
          break;
        offset += CHUNK_SIZE;
      }
      //msg ("Max file size: %li", bytes_written);
    }

    close (fd);
    if (!remove (file))
      fail ("Error, could not delete file %s", file);

    if (i == 0)
      initial_max_file_size = bytes_written;

    /* Must be at least min_max_size bytes. */
    if (bytes_written < min_max_size)
      fail ("Error, could only write %li < %li bytes", bytes_written, min_max_size);

    /* Should be the same number every time, whatever it is. */
    if (initial_max_file_size != bytes_written)
      fail ("Error, on the first iteration I was able to write a total of %li bytes. On this iteration I wrote %li bytes. Why do these differ?", initial_max_file_size, bytes_written);

  }

  return 0;
}
