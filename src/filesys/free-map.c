#include "filesys/free-map.h"
#include <bitmap.h>
#include <debug.h>
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"

static struct file *free_map_file;   /* Free map file. */
static struct bitmap *free_map;      /* Free map, one bit per sector. */

/* free_map changes from free_map_allocate and free_map_release are cached.
   They are written out on free_map_close and free_map_commit. */

/* Initializes the free map. */
void
free_map_init (void) 
{
  free_map = NULL;
  free_map_file = NULL;

  free_map = bitmap_create (block_size (fs_device));
  if (free_map == NULL)
    PANIC ("bitmap creation failed--file system device is too large");
  int i;
  for (i = 0; i < SECTORS_PER_INODE; i++)
  {
    /* Mark an inode's worth for the FREE_MAP and the ROOT_DIR. */
    bitmap_mark (free_map, FREE_MAP_SECTOR + i);
    bitmap_mark (free_map, ROOT_DIR_SECTOR + i);
    /* Make sure there's no overlap. */
    ASSERT (ROOT_DIR_SECTOR + i != FREE_MAP_SECTOR);
    ASSERT (FREE_MAP_SECTOR + i != ROOT_DIR_SECTOR);
  }
}

/* Allocates CNT consecutive sectors from the free map and stores
   the first into *SECTORP.
   Returns true if successful, false if not enough consecutive
   sectors were available. */ 
bool
free_map_allocate (size_t cnt, block_sector_t *sectorp)
{
  block_sector_t sector = bitmap_scan_and_flip (free_map, 0, cnt, false);
  if (sector != BITMAP_ERROR)
  {
    *sectorp = sector;
    return true;
  }
  return false;
}

/* Makes CNT sectors starting at SECTOR available for use. */
void
free_map_release (block_sector_t sector, size_t cnt)
{
  ASSERT (bitmap_all (free_map, sector, cnt));
  bitmap_set_multiple (free_map, sector, cnt, false);
}

/* If free map has been initialized, commits it to disk. */
void free_map_commit (void)
{
  /* Could be NULL during initialization. */
  if (free_map_file != NULL)
    bitmap_write (free_map, free_map_file);
}

/* Opens the free map file and reads it from disk. */
void
free_map_open (void) 
{
  free_map_file = file_open (inode_open (FREE_MAP_SECTOR));
  if (free_map_file == NULL)
    PANIC ("can't open free map");
  if (!bitmap_read (free_map, free_map_file))
    PANIC ("can't read free map");
}

/* Writes the free map to disk and closes the free map file. */
void
free_map_close (void) 
{
  file_close (free_map_file);
}

/* Creates a new free map file on disk and writes the free map to
   it. */
void
free_map_create (void) 
{
  /* Create inode. */
  if (!inode_create (FREE_MAP_SECTOR, INODE_FILE, bitmap_file_size (free_map)))
    PANIC ("free map creation failed");

  /* Write bitmap to file. */
  free_map_file = file_open (inode_open (FREE_MAP_SECTOR));
  if (free_map_file == NULL)
    PANIC ("can't open free map");
  if (!bitmap_write (free_map, free_map_file))
    PANIC ("can't write free map");
}
