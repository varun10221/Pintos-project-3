#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define ADDRESS_LEN sizeof(uint32_t)
#define INODE_N_ADDRESSES (INODE_SIZE - 3*ADDRESS_LEN)/ADDRESS_LEN /* Number of addresses an inode can store. */

#define INODE_N_INDIRECT_1_BLOCKS 16   /* Number of addresses of blocks of direct blocks. */
#define INODE_N_INDIRECT_2_BLOCKS 15   /* Number of addresses of INDIRECT_1 blocks. */
#define INODE_N_INDIRECT_3_BLOCKS 1    /* Number of addresses of INDIRECT_2 blocks. */
#define INODE_N_DIRECT_BLOCKS (INODE_N_ADDRESSES - INODE_N_INDIRECT_3_BLOCKS - INODE_N_INDIRECT_2_BLOCKS - INODE_N_INDIRECT_1_BLOCKS) /* Number of addresses of data blocks. */

/* For processing the info_and_cksum field present in metadata structures. */
#define METAINFO_MASK 0x7
struct metainfo_s
{
  int8_t info;
  unsigned cksum;
};

/* Inodes can be one of these types. */
enum inode_type
{
  INODE_FILE,
  INODE_DIRECTORY
};

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
  uint32_t info_and_cksum;   /* Bits 0-7 are an enum inode_type. Bits 8-31 are a cksum of the metadata in the inode. */
  unsigned magic;       /* Magic number. */
  off_t length;         /* File size in bytes. */
  /* INODE_N_ADDRESSES addresses. */
  uint32_t direct_blocks     [INODE_N_DIRECT_BLOCKS];     /* Each is the address of a block of data. */
  uint32_t indirect_1_blocks [INODE_N_INDIRECT_1_BLOCKS]; /* Each is the address of a block of direct blocks. */
  uint32_t indirect_2_blocks [INODE_N_INDIRECT_2_BLOCKS]; /* Each is the address of a block of indirect_1 blocks. */
  uint32_t indirect_3_blocks [INODE_N_INDIRECT_3_BLOCKS]; /* Each is the address of a block of indirect_2 blocks. */
};

/* On-disk indirect block.
   Must be exactly METADATA_BLOCKSIZE bytes long. */
#define INDIRECT_BLOCK_N_ADDRESSES (METADATA_BLOCKSIZE - 1*ADDRESS_LEN)/ADDRESS_LEN /* The number of addresses contained in an indirect blcok. */
struct indirect_block
{
  uint32_t info_and_cksum; /* Bits 0-7 are an integer for the indirect level. Bits 8-31 are a cksum of the addresses. */
  uint32_t blocks [INDIRECT_BLOCK_N_ADDRESSES]; /* The block addresses tracked by this indirect block. */
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
  };

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  if (pos < inode->data.length)
    return inode->data.direct_blocks[0] + pos / BLOCK_SECTOR_SIZE;
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails.
   TODO:should we use buffer cache for inode create? */
bool
inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      if (free_map_allocate (sectors, &disk_inode->direct_blocks[0])) 
        {
          block_write (fs_device, sector, disk_inode);
          if (sectors > 0) 
            {
              static char zeros[BLOCK_SECTOR_SIZE];
              size_t i;
              
              for (i = 0; i < sectors; i++) 
                block_write (fs_device, disk_inode->direct_blocks[0]+ i, zeros);
            }
          success = true; 
        } 
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
 
  /* Create or retrieve a cache_block */
  struct cache_block *cb = cache_get_block (inode->sector, 
                             CACHE_BLOCK_INODE, false);
  
  /* Read the data from cache_block */
  void *buffer = cache_read_block (cb);
  /*Do a memcpy from buffer in to inode data */
  memcpy (&inode->data, buffer, INODE_SIZE);
   
  cache_put_block (cb);
  
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        { 
          cache_discard (inode->sector, CACHE_BLOCK_INODE); 
          free_map_release (inode->sector, 1);
          free_map_release (inode->data.direct_blocks[0],
                            bytes_to_sectors (inode->data.length)); 
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;
     
      /* Reserve a cache block in buffer cache and save it . */
      struct cache_block * cb = cache_get_block (sector_idx , CACHE_BLOCK_INODE , false);
      ASSERT (cb != NULL);    

      /* cache source from which the date will be transferred 
         to the buffer */ 
      uint8_t *cache_src = cache_read_block (cb);
     
      /*copying contents on to buffer */
      memcpy (buffer + bytes_read , cache_src + sector_ofs , chunk_size);
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
  
     /*release the access to the block */
      cache_put_block (cb);
    }
  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;
      
      struct cache_block *cb = cache_get_block (sector_idx, CACHE_BLOCK_INODE, true);
      ASSERT (cb != NULL);
     
      /* cache dst to which contents will be written to */
       uint8_t *cache_dst = cache_read_block (cb);
      
      /* Copying contents from buffer to cache */
        memcpy (cache_dst + sector_ofs, buffer + bytes_written, chunk_size);   
        cache_mark_block_dirty (cb);
     
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    
      /* Release the cache_block */
      cache_put_block (cb);
    }

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}
