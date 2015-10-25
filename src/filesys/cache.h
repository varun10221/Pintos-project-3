#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <stdint.h>
#include <inttypes.h>
#include <stddef.h>





struct  buffer_cache_table;
struct  cache_block;

/*Basic life cycle */
void buffer_cache_table init (size_t);
void buffer_cache_table_destroy (void);

/* Getting and releasing access to cache blocks */
struct cache_block * cache_get_block (disk_sector_t , bool );
void cache_put_block (struct cache_block *);

/*Storing and releasing blocks in cache block table */
void * cache_read_block (struct cache_block *b);
void * cache_zero_block (struct cache_block *b);

/*Marking the state of cache block */
void cache_mark_block_dirty (struct cache_block *b);
void cache_mark_block_invalid (struct cache_block *b);

/*Periodic flushing of cache */
void buffer_cache_table_flush_data (void);
  





#endif /* filesys/cache.h */
