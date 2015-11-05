#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"


/* A directory. */
struct dir 
  {
    struct inode *inode;                /* Backing store. */
    off_t pos;                          /* Current position. */
    struct dir *parent_dir;             /* Parent dir to current_ dir */
    struct lock dir_lock [4];           /* Dir lock */
  };

/* A single directory entry. */
struct dir_entry 
  {
    block_sector_t inode_sector;        /* Sector number of header. */
    char name[NAME_MAX + 1];            /* Null terminated file name. */
    bool in_use;                        /* In use or free? */
  };



/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (block_sector_t sector, size_t entry_cnt)
{
  return inode_create (sector, INODE_DIRECTORY, entry_cnt * sizeof (struct dir_entry));
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
    {
      dir->inode = inode;
      dir->pos = 0;
    }

  else
    {
      inode_close (inode);
      free (dir);
      dir = NULL; 
    }

  if (dir)
  {  int i;
     for (i = 0; i < 4; i++)
     lock_init (&dir->dir_lock[i]);
  }
   return dir;
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }
  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode) 
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  int hash_number;

  /* Acquire an inode based on char name */
  hash_number = dir_hash_lock_acquire ((struct dir *) dir, name);
    
  if (lookup (dir, name, &e, NULL))
    *inode = inode_open (e.inode_sector);
  else
    *inode = NULL;

  /* Release the inode_lock */
   dir_hash_lock_release ((struct dir *) dir, hash_number);

  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, block_sector_t inode_sector)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);
  

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;
 
  /* Acquires a diretory lock */
  int hash_number = dir_hash_lock_acquire (dir, name); 
     
  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

 done:
  { dir_hash_lock_release (dir, hash_number); 
    return success;
  }
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);
  
  /* Acquire hash_lock */
  int hash_number;
  hash_number = dir_hash_lock_acquire (dir, name);

  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

 done:
  inode_close (inode);
  dir_hash_lock_release (dir, hash_number);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;

  int hash_number;
 
  /* Acquire the inode hash lock */
  hash_number = dir_hash_lock_acquire (dir, name);
   
  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
    {
      dir->pos += sizeof e;
      if (e.in_use)
        {
          strlcpy (name, e.name, NAME_MAX + 1);
          return true;
        } 
    }
  
  /* Release the inode_hash_lock */
   dir_hash_lock_release (dir, hash_number);

  return false;
}


 
/* Directory is the last directory in the path
   Accepts a char * (absolute / relative address and outputs
   another char * (directory name ) */
char *
dir_extract_directory_name (char *path)
{
  if (path == NULL)
    return path;
  
  /* No. of characters in path */
  int path_length = strlen (path);
  char path_buf [path_length];
  strlcpy (path_buf, path, path_length +1);

  /* Now split the path based on  '/' */
  volatile char * token , *save_ptr , *next_token;
 
   token = strtok_r(path_buf,"/",&save_ptr);
   if (token != NULL)
      {  next_token = strtok_r (NULL, "/", &save_ptr);

         /* TODO: should replace with a while, will make more sense */
    
         while (next_token != NULL)
         {
          token = next_token;
          next_token = strtok_r (NULL, "/", &save_ptr);
         }     
      } 

   return token;
}


/* Finds the directory from the name (path)
   and returns a pointer to it 
   TODO: synchronization yet to be handled */

struct dir *
dir_find_dir_from_path (const char *name )
{
 /* Return NULL if name is NULL */
 if (name == NULL) 
   return NULL;

 /* Return current directory if '.' */
 if (strcmp (name, ".") == 0)
   return thread_current()->current_dir;

 /* TODO : check if we need path_buf variable,if yes allocate it thru malloc */
 struct thread *t = thread_current ();
 int name_length = strlen (name);
 char *path_buf;
 path_buf = (char*) malloc (name_length * sizeof(char));
 
 /* Copy the input name/path in to path_buf */
 memcpy (path_buf, name, name_length +1);
 
 if (strcmp (path_buf, "..") == 0)
      return dir_retrieve_parent_directory (t->current_dir);

 else if (strcmp (path_buf, "/") == 0)
      return dir_open_root ();

 volatile char *token, *save_ptr;
 char *next_token;
 
 /* Tokenize the string with '/' to 
 *     extract the first portion of path */
 token = strtok_r (path_buf, "/", &save_ptr);
 next_token = NULL;
 
 struct dir *dir;
 
 /* Check if path begins with '/' (absolute path) */
 if (path_buf [0] == '/' || t->current_dir == NULL)
     dir = dir_open_root ();
 
 /* If the path is not absolute , then it must be relative */
 else dir = dir_reopen (t->current_dir);
  
 if (token != NULL)
     next_token = strtok_r (NULL, "/", &save_ptr);
 
 /* Directory traversal */
 while (next_token) /* Fails if next_token becomes null */
  {
    struct inode *inode;   
    /* TODO : Use a directory lookup to find files */
    
    /* Look up for the directory */
    if (! dir_lookup (dir, token, &inode))    
        return NULL;
    
    if (inode_is_directory (inode))
       {
          dir_close (dir);
          dir = dir_open (inode);
       }
    else inode_close (inode); /* Directory is found */    
 
    token = next_token;
    next_token = strtok_r (NULL, "/", &save_ptr);  
  }
 /* Found the required directory */
 return dir; 
}


/* Returns the Parent directory of the given dir,
 *    returns NULL in case of no parent (root) */
struct dir *
dir_retrieve_parent_directory (struct dir *dir)
{
  
  return dir->parent_dir;

}
  
/* Adds a parent directory to the exisiting struct dir */
void 
dir_add_parent_dir (struct dir *parent_dir)
{
  if (parent_dir == NULL)
      return;
  
  struct thread *t = thread_current ();
  struct dir *dir = t->current_dir;
  dir->parent_dir = parent_dir;
 
}


/* Returns the hash value based on 's'
   for acquiring the inode_lock, the value is ensured
   to lie between 0 and 3 */
int
dir_lock_compute_hash_number (const char *s)
{
  return (int) (hash_string (s) % 4);
}

/* API to acquire the hash_lock to perform directory operation */
int
dir_hash_lock_acquire (struct dir* dir, const char *s)
{
  int a;
  a = dir_lock_compute_hash_number (s);
  lock_acquire (&dir->dir_lock[a]);
  return a;
}

/* API to release the  dir_lock_that was acquired,
   Gets the dir and hash_number (computed based on its file_name)
   as input, hash_number is used becoz, in future if we have feature to 
   modify the filename,lock should not be lost */

void
dir_hash_lock_release (struct dir *dir, int hash_number)
{
  lock_release (&dir->dir_lock[hash_number]);
}



  
 
