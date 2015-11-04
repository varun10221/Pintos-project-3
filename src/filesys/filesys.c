#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

static struct lock filesys_mutex;

/* Some Internal functions */
struct dir * dir_retrieve_absolute_path (const char *);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");
  cache_init (fs_device);

  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();

  lock_init (&filesys_mutex);
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
  cache_destroy ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if an FS object named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
  block_sector_t inode_sector = 0;
  struct dir *dir = dir_open_root ();
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, INODE_FILE, initial_size)
                  && dir_add (dir, name, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  struct dir *dir = dir_open_root ();
  struct inode *inode = NULL;

  if (dir != NULL)
    dir_lookup (dir, name, &inode);
  dir_close (dir);

  return file_open (inode);
}

/* Creates a dir named NAME, with initial size 0
   Returns true if successful, false otherwise.
   Fails if an FS object named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create_dir (const char *name) 
{
  /* TODO */
 block_sector_t inode_sector = 0;
 char *dir_name = dir_extract_directory_name (name);
 struct dir *dir = dir_find_dir_from_path ();
 int initial_size = 0;
 bool success;
 /* Check if . or .. is passed to create directory, print an error
    message if true */
 if (strcmp (dir_name, ".") != 0 && strcmp (dir_name, ".") != 0)
     { 
      bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size)
                  && dir_add (dir, dir_name, inode_sector));
      
      if (!success && inode_sector != 0)
        free_map_release (inode_sector, 1);
      /* Create the link to parent_directory and to itself */
      char name [2] = ".";
      char *path = name;
      filesys_create_dir (path);
    
      char name_parent [3] = "..";
      char *path_parent = name_parent;
      filesys_create_dir (path_parent);
     } 
  else
     {
       if(strcmp (dir_name,".") == 0)
         {
          struct thread * t = thread_current ();
          struct dir * dir = t->current_dir;
          inode_sector = dir_get_inode (dir);
          dir_add (dir, dir_name, inode_sector);
         }
       else if (strcmp (dir_name,"..") == 0)
         {
           struct thread * t = thread_current ();
           struct dir *dir = dir_retrieve_parent_dir (t->current_dir);
          inode_sector = dir_get_inode (dir);
          dir_add (dir, dir_name, inode_sector);
         }
      }
  free (dir_name);
  dir_close (dir);
  return success;
}

/* Opens the dir with the given NAME.
   Returns the new dir if successful or a null pointer
   otherwise.
   Fails if no dir named NAME exists,
   or if an internal memory allocation fails. */
struct dir *
filesys_open_dir (const char *name)
{
  /* TODO */
  return NULL;
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  struct dir *dir = dir_open_root ();
  bool success = dir != NULL && dir_remove (dir, name);
  dir_close (dir); 

  return success;
}


/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}

/* P2: You should use synchronization to ensure that only one process 
   at a time is executing file system code. */
void
filesys_lock ()
{
//  lock_acquire (&filesys_mutex);
}

/* P2: You should use synchronization to ensure that only one process 
   at a time is executing file system code. */
void
filesys_unlock ()
{
  //lock_release (&filesys_mutex);
}



   
