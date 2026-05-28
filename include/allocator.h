#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
  STRATEGY_FIRST_FIT,
  STRATEGY_BEST_FIT,
  STRATEGY_SEGREGATED
} AllocStrategy;

void set_alloc_strategy(AllocStrategy strategy);

void *my_malloc(size_t size);
void my_free(void *ptr);

#endif
