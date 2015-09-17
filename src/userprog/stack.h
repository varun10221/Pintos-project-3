#ifndef USERPROG_STACK_H
#define USERPROG_STACK_H

#include <inttypes.h>
#include <stddef.h>

void * stack_push_string (void **sp, char *str);
void * stack_push_int (void **sp, int i);
void * stack_push_int32 (void **sp, int32_t i);
void * stack_push_ptr (void **sp, void *ptr);
void * stack_align (void **sp, size_t align);

void * stack_push_raw (void **sp, void *data, size_t n_bytes);

void * stack_pop (void **sp);

#endif /* userprog/stack.h */
