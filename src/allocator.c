#include "allocator.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct BlockMeta {
  size_t size;
  bool is_free;
  struct BlockMeta *next;
  struct BlockMeta *prev;
} BlockMeta;

#define META_SIZE sizeof(BlockMeta)

// Global state (in a real project, a mutex is required for thread-safety)
static BlockMeta *global_base = NULL;
static AllocStrategy current_strategy = STRATEGY_FIRST_FIT;

static BlockMeta *request_memory(BlockMeta *last, size_t size) {
    // Ideally, we should allocate memory in multiples of the page size (sysconf(_SC_PAGESIZE)).
    // For the time being, to keep things simple, we are allocating the exact size + metadata.
  void *request = mmap(NULL, size + META_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  if (request == MAP_FAILED)
    return NULL;

  BlockMeta *block = (BlockMeta *)request;
  block->size = size;
  block->is_free = false;
  block->next = NULL;
  block->prev = last;

  if (last) {
    last->next = block;
  }
  return block;
}

// First Fit
static BlockMeta *find_first_fit(BlockMeta **last, size_t size) {
  BlockMeta *current = global_base;
  while (current) {
    if (current->is_free && current->size >= size) {
      return current;
    }
    *last = current;
    current = current->next;
  }
  return NULL;
}

// Best Fit
static BlockMeta *find_best_fit(BlockMeta **last, size_t size) {
  BlockMeta *current = global_base;
  BlockMeta *best_fit = NULL;

  while (current) {
    if (current->is_free && current->size >= size) {
      if (!best_fit || current->size < best_fit->size) {
        best_fit = current;
      }
    }
    *last = current;
    current = current->next;
  }
  return best_fit;
}

void *my_malloc(size_t size) {
  if (size == 0)
    return NULL;

  size = (size + 7) & ~7;

  BlockMeta *block;

  if (!global_base) {
    block = request_memory(NULL, size);
    if (!block)
      return NULL;
    global_base = block;
  } else {
    BlockMeta *last = global_base;

    if (current_strategy == STRATEGY_FIRST_FIT) {
      block = find_first_fit(&last, size);
    } else if (current_strategy == STRATEGY_BEST_FIT) {
      block = find_best_fit(&last, size);
    } else {
      // Заглушка для Segregated List
      block = find_first_fit(&last, size);
    }

    if (!block) {
      block = request_memory(last, size);
      if (!block)
        return NULL;
    } else {
      block->is_free = false;
      // TODO: Split the block if it is too large.
    }
  }

  return (block + 1);
}

static BlockMeta *get_block_ptr(void *ptr) { return (BlockMeta *)ptr - 1; }

void my_free(void *ptr) {
  if (!ptr)
    return;

  BlockMeta *block = get_block_ptr(ptr);
  block->is_free = true;

  // TODO: Coalescing (merging) of adjacent free blocks
  // If block->prev is also free -> merge
  // If block->next is also free -> merge
}

void set_alloc_strategy(AllocStrategy strategy) { current_strategy = strategy; }

int main(void) {
  printf("Testing First Fit...\n");
  set_alloc_strategy(STRATEGY_FIRST_FIT);

  int *arr1 = (int *)my_malloc(100 * sizeof(int));
  int *arr2 = (int *)my_malloc(200 * sizeof(int));

  arr1[0] = 42;
  arr2[0] = 84;
  printf("arr1[0] = %d, arr2[0] = %d\n", arr1[0], arr2[0]);

  my_free(arr1);
  my_free(arr2);
  printf("Done!\n");

  return 0;
}
