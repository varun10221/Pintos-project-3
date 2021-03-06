#ifndef FILESYS_DIRECTORY_H
#define FILESYS_DIRECTORY_H

#include <stdbool.h>
#include <stddef.h>
#include "devices/block.h"

/* Maximum length of a file name component.
   This is the traditional UNIX maximum length.
   After directories are implemented, this maximum length may be
   retained, but much longer full path names must be allowed. */
#define NAME_MAX 14

struct inode;

/* Opening and closing directories. */
bool dir_create (block_sector_t sector, size_t entry_cnt);
struct dir *dir_open (struct inode *);
struct dir *dir_open_root (void);
struct dir *dir_reopen (struct dir *);
void dir_close (struct dir *);
struct inode *dir_get_inode (struct dir *);

/* Reading and writing. */
bool dir_lookup (const struct dir *, const char *name, struct inode **);
bool dir_add (struct dir *, const char *name, block_sector_t);
bool dir_remove (struct dir *, const char *name);
bool dir_readdir (struct dir *, char name[NAME_MAX + 1]);

/* Retrieving the directory */
struct dir * dir_retrieve_parent_directory (struct dir *);
char * dir_extract_directory_name (char *);
void dir_add_parent_dir (struct dir *);
struct dir * dir_find_dir_from_path (const char *);

/* Dir lock operations */
/* Internal directory functions */
int dir_hash_lock_acquire (struct dir *,const char *);
void dir_hash_lock_release (struct dir *, int);
int dir_lock_compute_hash_number (const char *);


#endif /* filesys/directory.h */
