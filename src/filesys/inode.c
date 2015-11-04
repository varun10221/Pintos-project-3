#include "filesys/inode.h"
#include <hash.h>
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include <stdlib.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/cache.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Identifies an inode or an indirect block. */
#define INODE_MAGIC 0x494e4f44
#define INDIRECT_MAGIC 0xabcd1234

#define ADDRESS_LEN sizeof(uint32_t)
#define INODE_N_ADDRESSES (INODE_SIZE - 4*ADDRESS_LEN)/ADDRESS_LEN /* Number of addresses an inode can store. */
#define DATA_IN_INODE_LENGTH (INODE_N_ADDRESSES * ADDRESS_LEN) /* If data-in-inode, this is the max file length. */

/* Whether a block is a direct block (points to data) or an indirect block (points to metadata). */
enum inode_block_type
{
  BLOCK_TYPE_DIRECT,
  BLOCK_TYPE_INDIRECT
};

/* Description of the IO we would like to perform. 
   Everything needed by inode_lookup_block_info.  */
struct io_info
{
  struct inode *inode; /* Inode to which we are doing IO. */
  off_t size;          /* Size of IO. */
  off_t offset;        /* Starting offset of IO. */
  bool write;          /* Are we writing? */
};

/* Description of the block to request from cache_get_block to fulfill a block_info_request. 
   Everything the caller needs from inode_lookup_block_info. */
struct block_info
{
  block_sector_t block_idx;
  bool new_block; /* Is a newly allocated block? If true, caller should zero out the block. */
};

/* Special values for a block_sector_t. These are safe because the disk size is nowhere near the max value of a block_sector_t. */
/* HOLE: sparse region of a file. */
#define HOLE -1
/* BEING_FILLED: another process is filling in a HOLE. */
#define BEING_FILLED -2

/* In case we want to divide info_and_cksum into 4-bit and 28-bit sections. */
#define METAINFO_MASK 0xF
struct meta_info
{
  /* NB We track both cksum and magic because they tell us something about
     the type of corruption. If magic is clobbered, the issue is a wild
     write or an incorrect read. If magic is OK but cksum is clobbered,
     the issue is likely an internal consistency error. */
  uint32_t info_and_cksum; /* Bits 0-7 are "info". Bits 8-31 are a cksum. */
  uint32_t magic;
  uint32_t indirection_level; /* 0 means addresses are to data blocks. 1 means addresses are to indirect blocks at indirection_level 0. etc. */
};

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
  /* Bits 0-7 of meta_info.info_and_cksum is an enum inode_type. Bits 8-31 are a cksum of the metadata in the inode. */
  struct meta_info meta_info;

  off_t length;               /* File size in bytes. */

  block_sector_t addresses [INODE_N_ADDRESSES]; /* The addresses tracked by this inode. Meaning is encoded in indirection_level. */
};

/* On-disk indirect block.
   Must be exactly METADATA_BLOCKSIZE bytes long. */
#define INDIRECT_BLOCK_N_ADDRESSES (METADATA_BLOCKSIZE - 3*ADDRESS_LEN)/ADDRESS_LEN /* The number of addresses contained in an indirect block. */
struct indirect_block
{
  /* Bits 0-7 of meta_info.info_and_cksum are reserved. Bits 8-31 are a cksum of the metadata in the indirect block. */
  struct meta_info meta_info;

  block_sector_t addresses [INDIRECT_BLOCK_N_ADDRESSES]; /* The addresses tracked by this indirect block. Meaning is encoded in indirection_level. */
};

/* In-memory inode. 
   Modifications to the fields of an inode* should be done under the lock.

   In general, multiple processes can do IO to an inode* concurrently. 
   Reads and writes can proceed in parallel, with contention resolved 
     by the buffer cache. 

   However, when an inode* is being grown (or shrunk, if truncate is supported), 
     the metadata is changing in major ways and exclusive access is required.
   Note that a sparse access does not constitute growth. Growth only occurs
     when inode_length() changes. */
struct inode 
  {
    struct hash_elem elem;              /* Element in open inode table. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
   
    struct lock lock;                   /* Mutex */
    int n_users;                        /* Number of active users (not growers). */
    struct condition done_being_grown;  /* Condition. Inode is unsafe while it is being grown. */
    struct condition done_being_used;   /* Condition. Growers can proceed when the last current user leaves. */
    bool is_being_grown;                /* Set by a grower. Readers wait on done_being_grown while there's a grower. */
    struct condition done_being_filled; /* Condition. If an address in inode_find_block is BEING_FILLED, wait on this. */
    struct lock inode_hash_lock[4];     /* Inode lock based on hash */
  };

/* Private inode_disk functions. */
static bool inode_create_empty (block_sector_t, enum inode_type);
static enum inode_type inode_disk_get_type (const struct inode_disk *);
static enum inode_type inode_get_type (const struct inode *);
static void inode_disk_read (block_sector_t, struct inode_disk *);
static void inode_disk_flush (block_sector_t, struct inode_disk *);
static void inode_disk_set_type (struct inode_disk *, enum inode_type);
static uint32_t inode_disk_calc_cksum (struct inode_disk *);
static uint32_t inode_disk_read_cksum (struct inode_disk *);
static void inode_disk_update_cksum (struct inode_disk *);
static void inode_disk_verify_cksum (struct inode_disk *);
static void inode_update_length (struct inode *, off_t);

/* Private inode functions. */
static void inode_lock (struct inode *);
static bool inode_locked_by_me (struct inode *);
static void inode_unlock (struct inode *);

static void inode_flush (struct inode *);
static void inode_lookup_block_info (struct io_info *, struct block_info *);
static void inode_find_block (struct io_info *, struct block_info *);
static void inode_fill_hole (struct inode *, uint32_t *, enum inode_block_type, int);
static int inode_get_direct_block_size (struct inode *);
static int inode_type_to_direct_block_size (enum inode_type);
static bool inode_grow (struct inode *, off_t);

static unsigned inode_hash_hash_func (const struct hash_elem *, void *);
static bool inode_hash_less_func (const struct hash_elem *, const struct hash_elem *, void *);
static void inode_set_hash (struct inode *, block_sector_t);
static bool inode_address_is_hole (block_sector_t);
static bool inode_address_is_being_filled (block_sector_t);

/* Private indirect block functions. */
static block_sector_t indirect_block_allocate (uint32_t); 
static void indirect_block_free (block_sector_t, int);
static void indirect_block_read (block_sector_t, struct indirect_block *);
static void indirect_block_flush (block_sector_t, struct indirect_block *);

static uint32_t indirect_block_calc_cksum (struct indirect_block *);
static uint32_t indirect_block_read_cksum (struct indirect_block *);
static void indirect_block_update_cksum (struct indirect_block *);
static void indirect_block_verify_cksum (struct indirect_block *);

/* Meta-info functions. */
static uint32_t meta_info_get_cksum (const struct meta_info *);
static void meta_info_set_cksum (struct meta_info *, uint32_t);
static uint8_t meta_info_get_info (const struct meta_info *);
static void meta_info_set_info (struct meta_info *, uint8_t);

/* Misc */
static off_t calc_addressable_bytes (int, int);
static block_sector_t direct_block_allocate (enum inode_type);

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct hash open_inodes;
/* Coarse-grained lock is unavoidable with this hash implementation. 
   Probably not a big deal since most operations should be reads. 
   A rw lock would be appropriate if one is available. */
static struct lock open_inodes_lock;

/* Lock order: 
     - first lock open_inodes
     - if needed, then lock an individual inode 
   If you need to lock multiple inodes, order them by sector
     and lock smallest to largest. */

static void lock_open_inodes (void);
static void unlock_open_inodes (void);

/* Initializes the inode module. */
void
inode_init (void) 
{
  hash_init (&open_inodes, inode_hash_hash_func, inode_hash_less_func, NULL);
  lock_init (&open_inodes_lock);
}

/* Initializes an inode of TYPE with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device. The file can grow beyond LENGTH. If LENGTH > 0,
   that many bytes will be pre-allocated.
   Returns true if successful.
   Returns false if memory or disk allocation fails. 
   The directory code is in charge of ensuring that processes don't
   see a file in mid-create. */
bool
inode_create (block_sector_t sector, enum inode_type type, off_t length)
{
  ASSERT (0 <= length);
  bool success = true;

  bool size_zero_success = inode_create_empty (sector, type);
  if (size_zero_success)
  {
    if (length)
    {
      /* Grow the file to the appropriate length by writing one null byte. */
      struct inode *ino = inode_open (sector);
      if (ino != NULL)
      {
        char data = 0;
        off_t n_written = inode_write_at (ino, &data, 1, length-1);
        inode_close (ino);
        if (n_written != 1)
          success = false;
      }
      else
        success = false;
    }
  }
  else
    success = false;

  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   If someone else already has this inode open, we return that inode*.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct hash_elem *e;
  struct inode *inode = NULL;

  struct inode dummy;
  inode_set_hash (&dummy, sector);

  lock_open_inodes ();

  /* Check whether this inode is already open. */
  e = hash_find (&open_inodes, &dummy.elem);
  if (e != NULL)
  {
    inode = hash_entry (e, struct inode, elem);
    inode_reopen (inode);
    goto UNLOCK_AND_RETURN;
  }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    goto UNLOCK_AND_RETURN;

  /* Initialize. */
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  inode->is_being_grown = false;
  lock_init (&inode->lock);
  cond_init (&inode->done_being_grown);
  cond_init (&inode->done_being_used);
  cond_init (&inode->done_being_filled);
  int i;
  for (i = 0; i < 4; i++)
    lock_init (&inode->inode_hash_lock[i]);
  inode->n_users = 0;
 
  inode_disk_read (inode->sector, &inode->data);

  /* Inode is ready. */
  hash_insert (&open_inodes, &inode->elem);
  
  UNLOCK_AND_RETURN:
    unlock_open_inodes ();
    return inode;
}

/* Lock the open_inodes table. */
static void
lock_open_inodes (void)
{
  lock_acquire (&open_inodes_lock);
}

/* Unlock the open_inodes table. */
static void
unlock_open_inodes (void)
{
  lock_release (&open_inodes_lock);
}

/* Lock this inode. */
static void
inode_lock (struct inode *ino)
{
  ASSERT (ino != NULL);
  lock_acquire (&ino->lock);
}

/* Return true if I have this CB locked, else false. */
static bool 
inode_locked_by_me (struct inode *inode)
{
  ASSERT (inode != NULL);
  return lock_held_by_current_thread (&inode->lock);
}


/* Unlock this inode. */
static void
inode_unlock (struct inode *inode)
{
  ASSERT (inode != NULL);
  ASSERT (inode_locked_by_me (inode));
  lock_release (&inode->lock);
}

/* Flush the on-disk version of INODE to its residence. 
   The cksum is updated on its way out. */
static void 
inode_flush (struct inode *inode)
{
  ASSERT (inode != NULL);
  inode_disk_flush (inode->sector, &inode->data);
}

/* Sets BLOCK_INFO to contain the information needed to satisfy 
   the request describd in IO_INFO.

   If IO_INFO->write, grows and/or allocates space for sparse blocks as needed.
   If a new block is allocated, BLOCK_INFO->new_block is set to true. 

   On failure, BLOCK_INFO->block_idx is set to HOLE.
     If IO_INFO->write, HOLE will only be returned if there is an allocation failure.
     If !IO_INFO->write, HOLE can be returned if the block at this offset has not been 
       allocated (sparse file) or if the offset is past the end of the file. 
       
   The inode is guaranteed to not change until the caller invokes inode_decr_users. 
   The caller must invoke inode_decr_users prior to a subsequent invocation of inode_lookup_block_info. 

   If writing, the caller should chunk his request so that IO_INFO->blocksize <=[META]DATA_BLOCKSIZE; i.e. he must be requesting data that fits within one data block.
   This prevents trying to grow the inode too much in one shot. */
static void
inode_lookup_block_info (struct io_info *io_info, struct block_info *block_info)
{
  ASSERT (io_info != NULL);
  ASSERT (block_info != NULL);

  struct inode *inode = io_info->inode;
  ASSERT (inode != NULL);

  int data_blocksize = inode_get_direct_block_size (inode); 
  if (io_info->write)
  {
    /* Ensure the write does not cross a block boundary. */
    int max_size = data_blocksize;
    off_t mod = io_info->offset % data_blocksize;
    if (mod) 
      /* Non-aligned write. Only go up to the end of this block. */
      max_size = data_blocksize - mod;
    ASSERT (io_info->size <= max_size);
  }

  inode_lock (inode);

  /* Wait for any growers to finish. */
  while (inode->is_being_grown)
    cond_wait (&inode->done_being_grown, &inode->lock);

  /* Determine whether or not inode is big enough to support IO_INFO. 
     Grow inode if need be. */
  off_t final_size = io_info->offset + io_info->size;
  bool big_enough = true;

  int n_addrs = INODE_N_ADDRESSES;
  /* Calculate the number of bytes addressable by each address. */
  int address_size = calc_addressable_bytes (inode_get_direct_block_size (inode), inode->data.meta_info.indirection_level);
  int max_addressable = n_addrs*address_size;

  if (io_info->write)
  {
    /* For write, grow up to final_size if needed. */
    if (max_addressable < final_size)
    {
      block_info->new_block = true;
      big_enough = inode_grow (inode, final_size);
    }
    else
      big_enough = true;
  }
  else
  {
    /* For read, only need to be able to read the first byte to proceed. */
    if (max_addressable < io_info->offset)
      big_enough = false;
    else
      big_enough = true;
  }

  /* The inode is now large enough to support our use, or we've encountered a nasty failure. */
  inode->n_users++;
  inode_unlock (inode);

  /* If growth failed or we are reading past EOF, we know we're past data-in-inode. */
  if (!big_enough)
  {
    block_info->block_idx = HOLE;
    return;
  }

  /* Inode is now large enough. Traverse it. */
  inode_find_block (io_info, block_info);
  return;
}

/* Helper for inode_lookup_block_info. 
   Inode must be large enough to handle the request.
   Populates BLOCK_INFO based on IO_INFO.
   Returns true if a mapping is found, false if IO_INFO->inode does not map IO_INFO->offset. 
     Can return false if IO_INFO->write and there is an allocation failure,
     or if !IO_INFO->write and there is no mapping. 
   Will lock and unlock IO_INFO->inode. */
static void
inode_find_block (struct io_info *io_info, struct block_info *block_info)
{
  ASSERT (io_info != NULL);
  ASSERT (block_info != NULL);

  /* Default values. */
  block_info->new_block = false;
  block_info->block_idx = HOLE;

  struct inode *inode = io_info->inode;
  ASSERT (inode != NULL);
  off_t orig_offset = io_info->offset;

  int direct_block_size = inode_get_direct_block_size (inode);

  struct meta_info *meta_info = &inode->data.meta_info;

  off_t rel_offset = orig_offset;
  block_sector_t *addresses = inode->data.addresses;

  struct indirect_block *indir_blk = (struct indirect_block *) malloc (sizeof(struct indirect_block));
  ASSERT (indir_blk != NULL);

  bool is_inode = true;

  block_sector_t meta_address = inode->sector;

  /* Repeat until addresses points to the actual data, not indirection. */ 
  while (0 < meta_info->indirection_level)
  {
    int n_addrs = (meta_info->magic == INODE_MAGIC ? INODE_N_ADDRESSES : INDIRECT_BLOCK_N_ADDRESSES);

    /* Calculate the number of bytes addressable by each address. */
    int address_size = calc_addressable_bytes (direct_block_size, meta_info->indirection_level);

    /* Index into addresses. */
    off_t addr = rel_offset / address_size;
    /* If not in range, inode is too small. */
    ASSERT (addr < n_addrs);

    inode_lock (inode);
    /* If someone else is trying to access this address too, wait until it is filled in. 
       Use while(): condition is shared by all fillers, so might be woken by an unrelated filler. */
    while (inode_address_is_being_filled (addresses[addr])) 
      cond_wait (&inode->done_being_filled, &inode->lock);

    if (inode_address_is_hole (addresses[addr]))
    {
      /* Expand if we're writing, else return false. */
      if (io_info->write)
      {
        /* Unlocks and re-locks inode. */
        inode_fill_hole (inode, &addresses[addr], BLOCK_TYPE_INDIRECT, meta_info->indirection_level - 1);

        if (inode_address_is_hole (addresses[addr]))
        {
          /* Could not allocate a block for this write. Bail out. */
          block_info->block_idx = HOLE;
          free (indir_blk);
          inode_unlock (inode);
          return;
        }
        else
        {
          /* We added a new address; flush to disk. */
          if (is_inode)
            inode_flush (inode);
          else
            indirect_block_flush (meta_address, indir_blk);
        }
      }
      else
      {
        /* Not writing, so a hole means we're done. */
        block_info->block_idx = HOLE;
        free (indir_blk);
        inode_unlock (inode);
        return;
      }
    }
    
    /* Read the appropriate block, update rel_offset, addresses, and meta_info, and continue. */
    meta_address = addresses[addr];
    indirect_block_read (meta_address, indir_blk);

    rel_offset -= addr * address_size; /* Relative to this indirect block. */
    meta_info = &indir_blk->meta_info;
    addresses = indir_blk->addresses;
    is_inode = false;

    inode_unlock (inode);
  }

  /* We've reached indirection level 0: addresses are of direct blocks. */
  int address_size = direct_block_size;
  int addr = rel_offset / address_size;

  inode_lock (inode);
  /* If someone else is trying to access this address too, wait until it is filled in. 
     Use while(): condition is shared by all fillers, so might be woken by an unrelated filler. */
  while (inode_address_is_being_filled (addresses[addr]))
    cond_wait (&inode->done_being_filled, &inode->lock);

  if (inode_address_is_hole (addresses[addr]))
  {
    if (io_info->write)
    {
      /* Unlocks and re-locks inode. */
      inode_fill_hole (inode, &addresses[addr], BLOCK_TYPE_DIRECT, 0);
      /* We added a new address; flush to disk. */
      if (!inode_address_is_hole (addresses[addr]))
      {
        block_info->new_block = true;
        /* We added a new address; flush to disk. */
        if (is_inode)
          inode_flush (inode);
        else
          indirect_block_flush (meta_address, indir_blk);
      }
    }
  }

  block_info->block_idx = addresses[addr];
  free (indir_blk);
  inode_unlock (inode);

  return; 
}

/* Fill HOLE in locked INODE of type BLOCK_TYPE.
   We unlock and then lock INODE.
   On success, HOLE is updated to a valid address.
   On failure, HOLE remains HOLE.
   For a block of type BLOCK_TYPE_INDIRECT, AUX is the indirection level. */
static void
inode_fill_hole (struct inode *inode, uint32_t *hole, enum inode_block_type block_type, int aux)
{
  ASSERT (inode != NULL);
  ASSERT (hole != NULL);
  ASSERT (inode_locked_by_me (inode));

  /* We'll fill it in. */
  *hole = BEING_FILLED;
  inode_unlock (inode);

  /* Fill in the hole. */ 
  if (block_type == BLOCK_TYPE_INDIRECT)
    *hole = indirect_block_allocate (aux);
  else
    *hole = direct_block_allocate (inode_get_type (inode));

  /* Done filling the hole (whether or not we succeeded). */
  inode_lock (inode);
  cond_broadcast (&inode->done_being_filled, &inode->lock);
}

/* Returns the size of the blocks pointed to by the direct blocks of INODE. */
static int
inode_get_direct_block_size (struct inode *inode)
{
  ASSERT (inode != NULL);
  return inode_type_to_direct_block_size (inode_get_type (inode));
}

/* Returns the size of the blocks pointed to by the direct blocks
   of an inode of type INODE_TYPE.
     INODE_FILE:      DATA_BLOCKSIZE
     INODE_DIRECTORY: METADATA_BLOCKSIZE */
static int
inode_type_to_direct_block_size (enum inode_type inode_type)
{
  switch (inode_type)
  {
    case INODE_FILE:
      return DATA_BLOCKSIZE;
    case INODE_DIRECTORY:
      return METADATA_BLOCKSIZE;
    default:
      NOT_REACHED ();
  }
  NOT_REACHED ();
}

/* Get the cksum of META_INFO. */
static uint32_t
meta_info_get_cksum (const struct meta_info *meta_info)
{
  ASSERT (meta_info != NULL);
  return meta_info->info_and_cksum & ~METAINFO_MASK;
}

/* Set the cksum of META_INFO. */
static void 
meta_info_set_cksum (struct meta_info *meta_info, uint32_t cksum)
{
  ASSERT (meta_info != NULL);
  ASSERT (! (cksum & METAINFO_MASK)); /* The info bits should be zero. */
  uint8_t info = meta_info_get_info (meta_info); 
  meta_info->info_and_cksum = cksum | info;
}

/* Get the info of META_INFO. */
static uint8_t
meta_info_get_info (const struct meta_info *meta_info)
{
  ASSERT (meta_info != NULL);
  uint8_t info = meta_info->info_and_cksum & METAINFO_MASK;
  return info;
}

/* Set the info of META_INFO. */
static void
meta_info_set_info (struct meta_info *meta_info, uint8_t info)
{
  ASSERT (meta_info != NULL);
  /* Keep everything above METAINFO_MASK, and then add in INFO. */
  meta_info->info_and_cksum = (meta_info->info_and_cksum & (~METAINFO_MASK)) | info;
}

/* Return the number of bytes that a pointer in a structure with indirection_level INDIRECTION_LEVEL
   can address, provided that indirection_level==0 pointers address BASE_BLOCKSIZE bytes. */
static off_t
calc_addressable_bytes (int direct_block_size, int indirection_level)
{
  int addressable_bytes = direct_block_size;
  int i;
  for (i = 0; i < indirection_level; i++)
    addressable_bytes *= INDIRECT_BLOCK_N_ADDRESSES;
  return addressable_bytes;
}

/* Companion to inode_lookup_block_info. The caller
   indicates that he is done using the inode. */
static void
inode_decr_users (struct inode *inode)
{
  ASSERT (inode != NULL);
  ASSERT (!inode_locked_by_me (inode));
  /* We're done, decrement n_users. */
  inode_lock (inode);
  ASSERT (0 <= inode->n_users);
  inode->n_users--;

  /* Wake up any pending growers. There should be at most 1. */
  if (inode->n_users == 0)
  {
    ASSERT (cond_n_waiters (&inode->done_being_used, &inode->lock) <= 1);
    cond_signal (&inode->done_being_used, &inode->lock);
  }
  inode_unlock (inode);
}

/* Grows locked INODE to POS. This does NOT modify
   the inode's length, but only its maximum possible length.
   Waits for any current users to finish. 
   Returns true on success, false on failure. 
   Unlocks inode and then re-locks it at the end. */
static bool
inode_grow (struct inode *inode, off_t pos)
{
  ASSERT (inode != NULL);
  ASSERT (inode_locked_by_me (inode));

  /* Must require growth at the time of calling. */
  ASSERT (inode_length (inode) < pos);
  /* Cannot be another grower due to condition in inode_lookup_block_info. */
  ASSERT (!inode->is_being_grown);
  inode->is_being_grown = true;

  /* Wait for all users to finish. */
  while (0 < inode->n_users)
    cond_wait (&inode->done_being_used, &inode->lock);
  ASSERT (inode->n_users == 0);
  /* Safe to unlock here and let prospective users "pile up" on done_being_grown. */
  inode_unlock (inode);

  /* No one should have beaten us to the punch. Only one grower at a time, and we waited for
     extant growers to finish before testing length prior to calling this function. */
  ASSERT (inode_length (inode) < pos);

  /* Calculate the indirection level required to address POS. */
  int direct_block_size = inode_get_direct_block_size (inode);
  int n_addrs = INODE_N_ADDRESSES;

  uint32_t curr_indirection = inode->data.meta_info.indirection_level;
  /* We can start at curr_indirection+1, but starting here lets us assert that we have to grow. */
  uint32_t indirection_required = curr_indirection;
  int address_size = calc_addressable_bytes (direct_block_size, indirection_required);
  int max_addressable = n_addrs*address_size;
  while (max_addressable < pos)
  {
    indirection_required++;
    address_size = calc_addressable_bytes (direct_block_size, indirection_required);
    max_addressable = n_addrs*address_size;
  }
  ASSERT (curr_indirection < indirection_required);

  /* We now have to grow by an arbitrary number of blocks.
     If allocation fails partway through, maintaining a list of newly allocated blocks will
     facilitate cleanup. */
  struct new_ind_blk
  {
    block_sector_t sector;
    struct indirect_block ind_blk;
    struct list_elem elem;
  };

  /* In order of indirection level, largest to smallest. */
  struct list new_ind_blks;
  list_init (&new_ind_blks);

  /* If false, we discard all elements of new_ind_blks. */
  bool success = true;

  /* A tmp block for use with indirect_block_read and indirect_block_flush. */
  block_sector_t tmp_ind_blk_sector = HOLE;
  struct indirect_block *tmp_ind_blk = (struct indirect_block *) malloc (sizeof(struct indirect_block));
  ASSERT (tmp_ind_blk != NULL);

  /* Allocate each new block, beginning at the lowest level of indirection.
     Each subsequent block references the previous one. */
  int n_new_blocks = indirection_required - curr_indirection;
  int i;
  block_sector_t prev_ind_blk_addr = HOLE;
  for (i = 0; i < n_new_blocks; i++)
  {
    struct new_ind_blk *nib = (struct new_ind_blk *) malloc (sizeof(struct new_ind_blk));
    ASSERT (nib != NULL);
    list_push_front (&new_ind_blks, &nib->elem);
    /* Some inefficiency, as we read the indirect block "from disk" twice: once here and once below when 
       setting the address to the previous indirect block.
       This uses clean APIs, though, and we expect the buffer cache to come to our rescue. */
    nib->sector = indirect_block_allocate (curr_indirection + i);
    tmp_ind_blk_sector = nib->sector;
    if (inode_address_is_hole (nib->sector))
    {
      success = false;
      break;
    }

    if (i == 0)
      prev_ind_blk_addr = nib->sector;
    else
    {
      /* If an intermediate block, point to the previously-allocated block. */
      ASSERT (!inode_address_is_hole (prev_ind_blk_addr) && !inode_address_is_being_filled (prev_ind_blk_addr)); 
      ASSERT (!inode_address_is_hole (tmp_ind_blk_sector) && !inode_address_is_being_filled (tmp_ind_blk_sector)); 
      indirect_block_read (nib->sector, tmp_ind_blk);
      tmp_ind_blk->addresses[0] = prev_ind_blk_addr;
      indirect_block_flush (tmp_ind_blk_sector, tmp_ind_blk);
    }
  }

  /* On success, copy inode contents to the bottom-most newly-allocated indirect block (which should be
     at the same level of indirection as the inode itself). Then point the inode to the upper-most newly-allocated
     indirect block, and set the inode's level of indirection to be one higher than that. */
  if (success)
  {
    /* If fewer addrs in indirect block, have to do some footwork. Should never be the case. */
    ASSERT (INODE_N_ADDRESSES < INDIRECT_BLOCK_N_ADDRESSES); 
    ASSERT (!list_empty (&new_ind_blks));
    /* Update bottom-most's contents. */
    struct new_ind_blk *nib = list_entry (list_back (&new_ind_blks), struct new_ind_blk, elem);
    indirect_block_read (nib->sector, tmp_ind_blk);
    ASSERT (tmp_ind_blk->meta_info.indirection_level == curr_indirection);
    uint32_t i;
    for (i = 0; i < INODE_N_ADDRESSES; i++)
    {
      tmp_ind_blk->addresses[i] = inode->data.addresses[i];
      inode->data.addresses[i] = HOLE;
    }
    indirect_block_flush (nib->sector, tmp_ind_blk);

    /* Update inode to point to upper-most. */ 
    nib = list_entry (list_back (&new_ind_blks), struct new_ind_blk, elem);
    inode->data.addresses[0] = nib->sector;
    inode->data.meta_info.indirection_level = indirection_required;
    inode_flush (inode);
  }

  free (tmp_ind_blk);

  /* Clean up all of the nib's. If we failed, also release the sectors in the free map. */
  struct list_elem *e;
  while (!list_empty (&new_ind_blks))
  {
    e = list_pop_front (&new_ind_blks);
    struct new_ind_blk *nib = list_entry (e, struct new_ind_blk, elem);
    ASSERT (nib != NULL);
    bool any_sectors_to_free = (!inode_address_is_hole (nib->sector) && !inode_address_is_being_filled (nib->sector));
    /* On failure, free sectors. */
    if (!success && any_sectors_to_free)
      free_map_release (nib->sector, SECTORS_PER_METADATA_BLOCK);
    free (nib);
  }

  /* Done, signal waiting users. */
  inode_lock (inode);
  inode->is_being_grown = false;
  cond_broadcast (&inode->done_being_grown, &inode->lock);

  return success;
}

/* Allocate an indirect block at INDIRECTION_LEVEL. 
   Returns the sector at which the allocated indirect block begins.
   Returns HOLE if allocation fails. */
static block_sector_t 
indirect_block_allocate (uint32_t indirection_level)
{
  unsigned i;
  struct indirect_block *ind_blk = NULL;

  /* If this assertion fails, the indirect block structure is not exactly
     one metadata block in size, and you should fix that. */
  ASSERT (sizeof *ind_blk == METADATA_BLOCKSIZE);

  /* Allocate a metadata block in memory. */
  ind_blk = (struct indirect_block *) malloc (sizeof *ind_blk);
  if (ind_blk == NULL)
    return HOLE;
    
  /* Allocate a metadata block on disk. */
  block_sector_t ind_blk_sector = HOLE;
  bool success = free_map_allocate (SECTORS_PER_METADATA_BLOCK, &ind_blk_sector);

  if (success)
  {
    /* Prep ind_blk. */
    ind_blk->meta_info.info_and_cksum = 0;
    ind_blk->meta_info.magic = INDIRECT_MAGIC;
    ind_blk->meta_info.indirection_level = indirection_level;
    for (i = 0; i < INDIRECT_BLOCK_N_ADDRESSES; i++)
      ind_blk->addresses[i] = HOLE;
    
    indirect_block_flush (ind_blk_sector, ind_blk);
  }
  else
    ind_blk_sector = HOLE;

  free (ind_blk);
  return ind_blk_sector;
}

/* Free the disk space allocated to the indirect block at SECTOR, and all of its children. 
   Intended to be used recursively. 
   Direct blocks consist of DIRECT_BLOCK_N_SECTORS sectors.
   Not thread safe. */
static void
indirect_block_free (block_sector_t sector, int direct_block_n_sectors)
{
  ASSERT (!inode_address_is_hole (sector) && !inode_address_is_being_filled (sector))

  struct indirect_block *ind_blk = (struct indirect_block *) malloc(sizeof *ind_blk);
  ASSERT (ind_blk != NULL);

  indirect_block_read (sector, ind_blk);
  unsigned i;
  /* Free children. */
  for (i = 0; i < INDIRECT_BLOCK_N_ADDRESSES; i++)
  {
    block_sector_t child_sector = ind_blk->addresses[i];
    if (inode_address_is_hole (child_sector) || inode_address_is_being_filled (child_sector))
      continue;

    if (ind_blk->meta_info.indirection_level == 0)
      free_map_release (child_sector, direct_block_n_sectors);
    else
      indirect_block_free (child_sector, direct_block_n_sectors);
  }
  /* Free self. */
  free_map_release (sector, SECTORS_PER_METADATA_BLOCK);
  free (ind_blk);
}


/* Read the indirect block that resides at SECTOR and write it to IND_BLK. */
static void
indirect_block_read (block_sector_t sector, struct indirect_block *ind_blk)
{
  ASSERT (ind_blk != NULL);

  struct cache_block *cb = cache_get_block (sector, CACHE_BLOCK_METADATA, false);
  ASSERT (cb != NULL);
  void *data = cache_read_block (cb);
  ASSERT (data != NULL);

  memcpy (ind_blk, data, sizeof (*ind_blk));
  cache_put_block (cb);

  ASSERT (ind_blk->meta_info.magic == INDIRECT_MAGIC);
  indirect_block_verify_cksum (ind_blk);
}

/* Flush IND_BLK to its residence at SECTOR.
   The cksum is updated on its way out. */
static void 
indirect_block_flush (block_sector_t sector, struct indirect_block *ind_blk)
{
  ASSERT (ind_blk != NULL);

  indirect_block_update_cksum (ind_blk);

  struct cache_block *cb = cache_get_block (sector, CACHE_BLOCK_METADATA, true);
  ASSERT (cb != NULL);
  void *data = cache_zero_block (cb);
  ASSERT (data != NULL);

  memcpy (data, ind_blk, sizeof *ind_blk);
  cache_mark_block_dirty (cb);
  cache_put_block (cb);
}

/* Calculate the cksum of this IND_BLK. */
static uint32_t
indirect_block_calc_cksum (struct indirect_block *ind_blk)
{
  ASSERT (ind_blk != NULL);
  uint32_t cksum = 0;
  uint8_t info = meta_info_get_info (&ind_blk->meta_info);
  cksum ^= hash_int (info);
  cksum ^= hash_bytes (&ind_blk->meta_info.magic, sizeof (struct indirect_block) - sizeof (uint32_t)); /* Everything after info_and_cksum. */

  cksum = (cksum & ~METAINFO_MASK); /* Zero out the type bits. */
  return cksum;
}

/* Return the recorded cksum of IND_BLK. */
static uint32_t
indirect_block_read_cksum (struct indirect_block *ind_blk)
{
  ASSERT (ind_blk != NULL);
  return meta_info_get_cksum (&ind_blk->meta_info);
}

/* Re-calculate and set the cksum of this IND_BLK. */
static void 
indirect_block_update_cksum (struct indirect_block *ind_blk)
{
  ASSERT (ind_blk != NULL);
  uint32_t cksum = indirect_block_calc_cksum (ind_blk);
  meta_info_set_cksum (&ind_blk->meta_info, cksum);
}

/* Asserts that the cksum of IND_BLK matches its current contents.
   Use immediately following read from disk. */
static void 
indirect_block_verify_cksum (struct indirect_block *ind_blk)
{
  ASSERT (ind_blk != NULL);
  uint32_t old  = indirect_block_read_cksum (ind_blk);
  uint32_t curr = indirect_block_calc_cksum (ind_blk);
  ASSERT (old == curr);
}

/* Allocate a direct block for an inode of the specified TYPE.
   The allocated block is not modified.
   Returns the sector at which the allocated block begins.
   Returns HOLE if allocation fails. */
static block_sector_t 
direct_block_allocate (enum inode_type type)
{
  int blocksize = inode_type_to_direct_block_size (type);
  int n_sectors = blocksize / BLOCK_SECTOR_SIZE;
  ASSERT (0 < blocksize);
  ASSERT (0 < n_sectors);
    
  /* Allocate a direct block. */
  block_sector_t blk_sector = HOLE;
  bool success = free_map_allocate (n_sectors, &blk_sector);

  if (success)
    return blk_sector;
  return HOLE;
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
   If this was the last reference to INODE:
     - frees its memory
     - If INODE was also a removed inode, frees its blocks. 
     - Else, commits them using free_map_commit. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  lock_open_inodes ();
  inode_lock (inode);

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
  {
    /* Remove from inode list. Now noone else can find INODE, so we can drop our locks. */ 
    hash_delete (&open_inodes, &inode->elem);
    inode_unlock (inode);
    unlock_open_inodes ();

    /* Deallocate blocks if removed. */
    if (inode->removed) 
    { 
      struct inode_disk *ino_d = &inode->data;
      int direct_block_n_sectors = inode_get_direct_block_size (inode) / BLOCK_SECTOR_SIZE;
      unsigned i;
      /* Free children. */
      for (i = 0; i < INODE_N_ADDRESSES; i++)
      {
        block_sector_t child_sector = ino_d->addresses[i];
        if (inode_address_is_hole (child_sector) || inode_address_is_being_filled (child_sector))
          continue;

        if (ino_d->meta_info.indirection_level == 0)
          free_map_release (child_sector, direct_block_n_sectors);
        else
          indirect_block_free (child_sector, direct_block_n_sectors);
      }
      /* Free self. */
      free_map_release (inode->sector, INODE_SIZE/BLOCK_SECTOR_SIZE);
    }
    else
      /* Flush any remaining changes. */
      inode_flush (inode);

    /* Commit any pending changes in the free map. */
    free_map_commit ();

    free (inode); 
  }
  else
  {
    inode_unlock (inode);
    unlock_open_inodes ();
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
  ASSERT (inode != NULL);
  ASSERT (buffer_ != NULL);

  uint8_t *buffer = buffer_;

  off_t bytes_read = 0;

  enum inode_type inode_type = inode_get_type (inode);
  enum cache_block_type cb_type;
  int blocksize = -1;
  switch (inode_type)
  {
    case INODE_FILE:
      cb_type = CACHE_BLOCK_DATA;
      blocksize = DATA_BLOCKSIZE;
      break;
    case INODE_DIRECTORY:
      cb_type = CACHE_BLOCK_METADATA;
      blocksize = METADATA_BLOCKSIZE;
      break;
    default:
      NOT_REACHED ();
  }
  ASSERT (0 < blocksize);

  struct io_info io_info;
  io_info.inode = inode;
  struct block_info block_info;

  while (size > 0) 
  {
    io_info.size = size;
    io_info.offset = offset;
    io_info.write = false;

    /* Block to read. inode's user count is incremented. */
    inode_lookup_block_info (&io_info, &block_info);
    bool is_allocated = !inode_address_is_hole (block_info.block_idx);

    /* Shorter names for contents. */
    block_sector_t block_idx = block_info.block_idx;
    int block_ofs = offset % blocksize;

    /* Bytes left in inode, bytes left in block, lesser of the two. */
    off_t inode_left = inode_length (inode) - offset;
    int block_left = blocksize - block_ofs;
    int min_left = MIN (inode_left, block_left);

    /* Number of bytes to copy out of this block. */
    int chunk_size = MIN (size, min_left);
    if (chunk_size <= 0)
    {
      /* Done with this inode for now. */
      inode_decr_users (inode);
      break;
    }

    if (is_allocated)
    {
      /* Get block from disk. */
      struct cache_block *cb = cache_get_block (block_idx, cb_type, false);
      ASSERT (cb != NULL);    
      uint8_t *data = cache_read_block (cb);
      ASSERT (data != NULL);
      memcpy (buffer + bytes_read, data + block_ofs, chunk_size);
      cache_put_block (cb);
    }
    else
      /* Sparse block or past EOF. Read returns all nulls. */
      memset (buffer + bytes_read, 0, chunk_size);
    
    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;

    /* Done with this inode for now. */
    inode_decr_users (inode);
  }

  return bytes_read;
}

/* Returns true if ADDRESS is a hole, else false. */
static bool
inode_address_is_hole (block_sector_t address)
{
  return (address == (block_sector_t) HOLE);
}

/* Returns true if ADDRESS is being filled, else false. */
static bool
inode_address_is_being_filled (block_sector_t address)
{
  return (address == (block_sector_t) BEING_FILLED);
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if maximum file size is reached or an error occurs. */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size, off_t offset) 
{
  ASSERT (inode != NULL);
  ASSERT (buffer_ != NULL);

  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;

  enum cache_block_type cb_type;
  int blocksize = inode_get_direct_block_size (inode);
  if (blocksize == DATA_BLOCKSIZE)
    cb_type = CACHE_BLOCK_DATA;
  else if (blocksize == METADATA_BLOCKSIZE)
    cb_type = CACHE_BLOCK_METADATA;
  else
    NOT_REACHED ();

  struct io_info io_info;
  io_info.inode = inode;
  struct block_info block_info;

  while (size > 0) 
  {
    io_info.offset = offset;
    io_info.size = size;
    /* Chunk IO requests to avoid growing too much at once. */
    off_t mod = io_info.offset % blocksize;
    if (mod)
      /* Non-aligned write. Only go up to the end of this block. */
      io_info.size = blocksize - mod;
    io_info.size = MIN (size, io_info.size);
    io_info.write = true;

    /* Block to write. inode's user count is incremented. */
    inode_lookup_block_info (&io_info, &block_info);
    bool allocation_failed = inode_address_is_hole (block_info.block_idx);
    if (allocation_failed)
    {
      /* Internal failure of some kind, bail out. 
         If this was a sparse file, it's conceivable that advancing might drop 
         us onto an already-allocated region where we would be able to write SOMETHING. 
         However, this is grasping at straws and would produce confusing behavior. */
      /* Done with this inode for now. */
      inode_decr_users (inode);
      break;
    }

    /* Shorter names for contents. */
    block_sector_t block_idx = block_info.block_idx;
    int block_ofs = offset % blocksize;

    /* Number of bytes to write into this block. */
    int chunk_size = io_info.size;
    if (chunk_size <= 0)
      break;
    
    struct cache_block *cb = cache_get_block (block_idx, cb_type, true);
    ASSERT (cb != NULL);
   
    /* If we're writing an entire block or a new block, overwrite the
         existing content. This prevents the leak of old data in inode_read_at.
       If the sector contains data before or after the chunk we're writing, 
         then we need to read in the sector first and modify the appropriate portion. */ 
    uint8_t *data = NULL;
    if (chunk_size == blocksize || block_info.new_block)
      /* New or full block, so overwrite. */
      data = cache_zero_block (cb);
    else
      /* Not a full block, so read-modify-write. */
      data = cache_read_block (cb);
    ASSERT (data != NULL);
    
    /* Copy contents from buffer to cache */
    memcpy (data + block_ofs, buffer + bytes_written, chunk_size);   
    cache_mark_block_dirty (cb);
    /* Release the cache_block */
    cache_put_block (cb);

    /* Update size if needed. */
    off_t final_byte = offset + chunk_size;
    if (inode->data.length < final_byte)
      inode_update_length (inode, final_byte);

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;

    /* Done with this inode for now. */
    inode_decr_users (inode);
  }

  return bytes_written;
}

/* If the current size of INODE < SIZE, update it to SIZE.
   Locks and unlocks INODE. */
static void
inode_update_length (struct inode *inode, off_t size)
{
  ASSERT (inode != NULL);
  ASSERT (!inode_locked_by_me (inode));
  ASSERT (0 < size);

  /* We may need to update size, so lock and double-check first. */
  inode_lock (inode);
  if (inode->data.length < size)
  {
    inode->data.length = size;
    inode_flush (inode);
  }
  inode_unlock (inode);
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  ASSERT (inode != NULL);

  inode_lock (inode);

  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);

  inode_unlock (inode);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode != NULL);

  inode_lock (inode);

  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;

  inode_unlock (inode);
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  ASSERT (inode != NULL);
  return inode->data.length;
}

/* Returns the type of INODE. */
static enum inode_type
inode_get_type (const struct inode *inode)
{
  ASSERT (inode != NULL);
  return inode_disk_get_type (&inode->data);
}

/* Read the inode_disk that resides at SECTOR and write it to INO_DISK. */
static void 
inode_disk_read (block_sector_t sector, struct inode_disk *ino_disk)
{
  ASSERT (ino_disk != NULL);

  struct cache_block *cb = cache_get_block (sector, CACHE_BLOCK_INODE, false);
  void *buffer = cache_read_block (cb);
  memcpy (ino_disk, buffer, INODE_SIZE);
  cache_put_block (cb);

  ASSERT (ino_disk->meta_info.magic == INODE_MAGIC);
  inode_disk_verify_cksum (ino_disk);
}


/* Flush INO_DISK to its residence in SECTOR.
   The cksum is updated on its way out. */
static void 
inode_disk_flush (block_sector_t sector, struct inode_disk *ino_disk)
{
  ASSERT (ino_disk != NULL);
  inode_disk_update_cksum (ino_disk);

  struct cache_block *cb = cache_get_block (sector, CACHE_BLOCK_INODE, true);
  ASSERT (cb != NULL);
  void *data = cache_zero_block (cb);
  ASSERT (data != NULL);

  memcpy (data, ino_disk, sizeof *ino_disk);
  cache_mark_block_dirty (cb);
  cache_put_block (cb);
}


/* Set the type of this INO_D. */
static void 
inode_disk_set_type (struct inode_disk *ino_d, enum inode_type type)
{
  ASSERT (ino_d != NULL);
  meta_info_set_info (&ino_d->meta_info, type);
}

/* Create an empty inode of the specified TYPE at sector SECTOR. 
   Return true on success, false on failure (memory allocation failure). */ 
static bool
inode_create_empty (block_sector_t sector, enum inode_type type)
{
  bool success = false;
  struct inode_disk *ino_d = NULL;
  unsigned i;
  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *ino_d == BLOCK_SECTOR_SIZE);

  ino_d = (struct inode_disk *) malloc (sizeof *ino_d);
  if (ino_d != NULL)
  {
    /* Prep ino_d. */
    ino_d->meta_info.info_and_cksum = 0;
    inode_disk_set_type (ino_d, type);
    ino_d->length = 0;
    ino_d->meta_info.magic = INODE_MAGIC;
    ino_d->meta_info.indirection_level = 0; /* Each address points to data blocks. */
    for (i = 0; i < INODE_N_ADDRESSES; i++)
      ino_d->addresses[i] = HOLE;
    
    inode_disk_flush (sector, ino_d);

    success = true; 
    free (ino_d);
  }

  return success;
}

/* Get the type of this INO_D. */
static enum inode_type
inode_disk_get_type (const struct inode_disk *ino_d)
{
  ASSERT (ino_d != NULL);
  return meta_info_get_info (&ino_d->meta_info);
}

/* Calculate the cksum of this INO_D. */
static uint32_t
inode_disk_calc_cksum (struct inode_disk *ino_d)
{
  ASSERT (ino_d != NULL);
  uint32_t cksum = 0;
  enum inode_type type = meta_info_get_info (&ino_d->meta_info);
  cksum ^= hash_int (type);
  cksum ^= hash_bytes (&ino_d->meta_info.magic, sizeof (struct inode_disk) - sizeof (uint32_t)); /* Everything after info_and_cksum. */

  cksum = (cksum & ~METAINFO_MASK); /* Zero out the type bits. */
  return cksum;
}

/* Return the recorded cksum of INO_D. */
static uint32_t
inode_disk_read_cksum (struct inode_disk *ino_d)
{
  ASSERT (ino_d != NULL);
  return meta_info_get_cksum (&ino_d->meta_info);
}

/* Re-calculate and set the cksum of this INO_D. */
static void
inode_disk_update_cksum (struct inode_disk *ino_d)
{
  ASSERT (ino_d != NULL);
  enum inode_type type = inode_disk_get_type (ino_d);
  unsigned cksum = inode_disk_calc_cksum (ino_d);
  ino_d->meta_info.info_and_cksum = cksum | type;
}

/* Asserts that the cksum of INO_D matches its current contents.
   Use immediately following read from disk. */
static void 
inode_disk_verify_cksum (struct inode_disk *ino_d)
{
  ASSERT (ino_d != NULL);
  uint32_t old  = inode_disk_read_cksum (ino_d);
  uint32_t curr = inode_disk_calc_cksum (ino_d);
  ASSERT (old == curr);
}

/* Hash func for use with open_inodes. */
static unsigned 
inode_hash_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  ASSERT (e != NULL);
  struct inode *ino = hash_entry (e, struct inode, elem);
  return hash_bytes (&ino->sector, sizeof (ino->sector));
}

/* Hash func for use with open_inodes. 
   Returns true if a < b, else false. */
static bool 
inode_hash_less_func (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED)
{
  if (a == NULL)
    return true;
  else if (b == NULL)
    return false;

  struct inode *a_ino = hash_entry (a, struct inode, elem);
  struct inode *b_ino = hash_entry (b, struct inode, elem);
  return a_ino->sector < b_ino->sector;
}

/* Set INO to hash to an inode with this sector. */
static void
inode_set_hash (struct inode *ino, block_sector_t sector)
{
  ASSERT (ino != NULL);
  memset (ino, 0, sizeof(struct inode));
  ino->sector = sector;
}

/* Returns if INODE is a directory. */
bool
inode_is_directory (const struct inode *inode)
{
  ASSERT (inode != NULL);
  enum inode_type type = inode_get_type (inode);
  return (type == INODE_DIRECTORY);
}


/* Returns the hash value based on 's'
   for acquiring the inode_lock, the value is ensured
   to lie between 0 and 3 */
int
inode_lock_compute_hash_number (const char *s)
{
  return (int) (hash_string (s) % 4);
}

/* API to acquire the hash_lock to perform directory operation */
int
inode_hash_lock_acquire (struct inode *inode, const char *s)
{
  ASSERT (inode != NULL);
  int a;
  a = inode_lock_compute_hash_number (s);
  lock_acquire (&inode->inode_hash_lock[a]);
  return a;
}

/* API to release the  dir_lock_that was acquired,
   Gets the dir and hash_number (computed based on its file_name)
   as input, hash_number is used becoz, in future if we have feature to 
   modify the filename,lock should not be lost */

void
inode_hash_lock_release (struct inode *inode, int hash_number)
{
  ASSERT (inode != NULL);
  lock_release (&inode->inode_hash_lock[hash_number]);
}


