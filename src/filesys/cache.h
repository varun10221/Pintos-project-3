#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include <stdbool.h>

struct buffer_cache_table;
struct cache_block;

/* Basic life cycle. */
void cache_init (void);
void cache_destroy (void);

/* Getting and releasing access to cache blocks. */
struct cache_block * cache_get_block (block_sector_t, bool);
void cache_put_block (struct cache_block *);

/* Storing and releasing blocks in cache block table. */
void * cache_read_block (struct cache_block *);
void * cache_zero_block (struct cache_block *);

/* Marking the state of cache block */
void cache_mark_block_dirty (struct cache_block *);
void cache_readahead (block_sector_t, enum cache_block_type);

/* Periodic flushing of cache */
void cache_flush (void);
  
#endif /* filesys/cache.h */
