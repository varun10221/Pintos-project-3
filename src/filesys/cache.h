#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

struct cache_block;

void cache_init (void);
void cache_destroy (void);

void cache_flush (void);

#endif /* filesys/cache.h */
