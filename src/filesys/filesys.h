#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"

/* Sectors of system file inodes. */
#define FREE_MAP_SECTOR 0       /* Free map file inode sector. */
#define ROOT_DIR_SECTOR 1       /* Root directory file inode sector. */

#define SECTORS_PER_DATA_BLOCK 8     /* 4KB. */
#define SECTORS_PER_METADATA_BLOCK 4 /* 2KB. */

#define DATA_BLOCKSIZE     SECTORS_PER_DATA_BLOCK*BLOCK_SECTOR_SIZE
#define METADATA_BLOCKSIZE SECTORS_PER_METADATA_BLOCK*BLOCK_SECTOR_SIZE


/* Block device that contains the file system. */
struct block *fs_device;

void filesys_init (bool format);
void filesys_done (void);
bool filesys_create (const char *name, off_t initial_size);
struct file *filesys_open (const char *name);
bool filesys_create_dir (const char *name);
struct dir *filesys_open_dir (const char *name);
bool filesys_remove (const char *name);

void filesys_lock (void);
void filesys_unlock (void);

#endif /* filesys/filesys.h */
