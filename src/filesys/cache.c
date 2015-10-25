#include "cache.h"



#define CACHE_FLUSH_ INTERVAL 1  /*keeping it as 1s for now */

/*The buffer_cache_table will be global
 and will be used to store file blocks */
struct buffer_cache_table
{
  uint32_t n_free_cache_blocks; /*No. of free blocks ,may use int8_t */
  uint32_t n_total_cache_blocks; /*Total no. of cache blocks in table */
  struct cache_block  *cache_block; /*Array for cache_table,table
       utmost will hold 64 sectors wortth of data */
  struct lock buffer_cache_lock; /*Incase , if any atomic operations needs to be performed */

}

/*Entry in Buffer_acche_table */ /*can be named as block maybe ?*/
struct cache_block
{
  block_sector_t block_sector_id; /*sector id */
  bool is_dirty;                  /*to check if the block has been written to */
  bool is_valid;                  /*If the data is still valid or has been invalidated */
  uint32_t readers_count;        /*No. of readers , reading from the block */
  uint32_t writers_count;        /*No. of writers writing to the block */
  uint32_t pending_requests;     /*Pending requests waiting due to read/write regulation */
  struct lock cache_block_lock; /*lock on this particular block alone */
}

