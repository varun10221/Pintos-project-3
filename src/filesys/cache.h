#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include <stdbool.h>

struct buffer_cache_table;
struct cache_block;

enum cache_block_type
{
  CACHE_BLOCK_INODE,   /* This cache block holds an inode. */
  CACHE_BLOCK_DATA,    /* This cache block holds a data block. */
  CACHE_BLOCK_METADATA /* This cache block holds a metadata block. */
};

/* Basic life cycle. */
void cache_init (struct block *);
void cache_destroy (void);

/* Getting and releasing access to cache blocks. 

   Usage: Call cache_get_block and cache_put_block in 
   successive pairs. You cannot get a new block before put'ing the
   previous block. */
struct cache_block * cache_get_block (block_sector_t, enum cache_block_type, bool);
void cache_put_block (struct cache_block *);

/* Interacting with cache blocks. */
void * cache_read_block (struct cache_block *);
void * cache_zero_block (struct cache_block *);
void cache_mark_block_dirty (struct cache_block *);
void cache_readahead (block_sector_t, enum cache_block_type);

/* Periodic flushing of cache */
void cache_flush (void);
  
#endif /* filesys/cache.h */
