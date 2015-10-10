#include "vm/page.h"

struct ro_shared_segment_table ro_shared_segment_table;

/* Initialize the ro shared segment table. Not thread safe. Should be called once. 
   TODO */
void 
ro_shared_segment_table_init (void)
{
}

/* Destroy the ro shared segment table. Not thread safe. Should be called once. 
   TODO */
void 
ro_shared_segment_table_destroy (void)
{
}
