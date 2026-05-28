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
  struct BlockMeta *next_free;
  struct BlockMeta *prev_free;
} BlockMeta;

#define META_SIZE sizeof(BlockMeta)

// Global state (in a real project, a mutex is required for thread-safety)
static BlockMeta *global_base = NULL;
static AllocStrategy current_strategy = STRATEGY_FIRST_FIT;

// Segregated List (SLUB-like) Structure
#define NUM_CLASSES 10
static size_t class_sizes[NUM_CLASSES] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
static BlockMeta *segregated_lists[NUM_CLASSES] = {NULL};

static int get_class_index(size_t size) {
  for (int i = 0; i < NUM_CLASSES; i++) {
    if (size <= class_sizes[i]) return i;
  }
  return -1;
}

static void add_to_segregated_list(BlockMeta *block) {
  int idx = get_class_index(block->size);
  if (idx == -1) return;

  block->next_free = segregated_lists[idx];
  block->prev_free = NULL;

  if (segregated_lists[idx]) {
    segregated_lists[idx]->prev_free = block;
  }
  segregated_lists[idx] = block;
}

static void remove_from_segregated_list(BlockMeta *block) {
  int idx = get_class_index(block->size);
  if (idx == -1) return;

  if (block->prev_free) {
    block->prev_free->next_free = block->next_free;
  } else {
    segregated_lists[idx] = block->next_free;
  }

  if (block->next_free) {
    block->next_free->prev_free = block->prev_free;
  }

  block->next_free = NULL;
  block->prev_free = NULL;
}

static BlockMeta *request_memory(BlockMeta *last, size_t size) {
  void *request = mmap(NULL, size + META_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  if (request == MAP_FAILED) return NULL;

  BlockMeta *block = (BlockMeta *)request;
  block->size = size;
  block->is_free = false;
  block->next = NULL;
  block->prev = last;
  block->next_free = NULL;
  block->prev_free = NULL;

  if (last) {
    last->next = block;
  }
  return block;
}

// First Fit
static BlockMeta *find_first_fit(BlockMeta **last, size_t size) {
  BlockMeta *current = global_base;
  while (current) {
    if (current->is_free && current->size >= size) return current;
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

// Segregated Fit
static BlockMeta *find_segregated_fit(BlockMeta **last, size_t size) {
  int idx = get_class_index(size);
  if (idx == -1) return NULL;

  if (segregated_lists[idx]) {
    BlockMeta *block = segregated_lists[idx];
    remove_from_segregated_list(block);
    return block;
  }

  size_t chunk_size = class_sizes[idx];
  size_t block_total_size = chunk_size + META_SIZE;
  size_t page_size = sysconf(_SC_PAGESIZE);

  if (page_size < block_total_size) {
      page_size = ((block_total_size / page_size) + 1) * page_size;
  }

  int num_chunks = page_size / block_total_size;
  if (num_chunks == 0) return NULL;

  void *slab = mmap(NULL, num_chunks * block_total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (slab == MAP_FAILED) return NULL;

  BlockMeta *tail = global_base;
  if (tail) {
      while (tail->next) tail = tail->next;
  }

  for (int i = 0; i < num_chunks; i++) {
    BlockMeta *block = (BlockMeta *)((char *)slab + i * block_total_size);
    block->size = chunk_size;
    block->is_free = true;
    block->next_free = NULL;
    block->prev_free = NULL;

    block->prev = tail;
    block->next = NULL;
    if (tail) {
      tail->next = block;
    } else {
      global_base = block;
    }
    tail = block;

    add_to_segregated_list(block);
  }

  if (last) *last = tail;

  BlockMeta *result = segregated_lists[idx];
  remove_from_segregated_list(result);
  return result;
}

void *my_malloc(size_t size) {
  if (size == 0) return NULL;
  size = (size + 7) & ~7;

  BlockMeta *block = NULL;
  BlockMeta *last = NULL;

  if (current_strategy == STRATEGY_SEGREGATED) {
    block = find_segregated_fit(&last, size);
  } else {
    if (!global_base) {
      block = request_memory(NULL, size);
      if (!block) return NULL;
      global_base = block;
      return (block + 1);
    }

    last = global_base;
    if (current_strategy == STRATEGY_FIRST_FIT) {
      block = find_first_fit(&last, size);
    } else if (current_strategy == STRATEGY_BEST_FIT) {
      block = find_best_fit(&last, size);
    }
  }

  if (!block) {
    if (global_base && !last) {
      last = global_base;
      while (last->next) last = last->next;
    }
    block = request_memory(last, size);
    if (!block) return NULL;
    if (!global_base) global_base = block;
  } else {
    block->is_free = false;
  }

  return (block + 1);
}

static BlockMeta *get_block_ptr(void *ptr) { return (BlockMeta *)ptr - 1; }

void my_free(void *ptr) {
  if (!ptr) return;

  BlockMeta *block = get_block_ptr(ptr);
  block->is_free = true;

  if (current_strategy == STRATEGY_SEGREGATED) {
      add_to_segregated_list(block);
  } else {
      // TODO: Coalescing (merging) of adjacent free blocks for generic strategies
  }
}

void set_alloc_strategy(AllocStrategy strategy) { current_strategy = strategy; }

int main(void) {
  printf("Testing Segregated Free Lists (SLUB-like)...\n");
  set_alloc_strategy(STRATEGY_SEGREGATED);

  int *arr1 = (int *)my_malloc(100 * sizeof(int));
  int *arr2 = (int *)my_malloc(200 * sizeof(int));

  int *arr3 = (int *)my_malloc(100 * sizeof(int));

  arr1[0] = 42;
  arr2[0] = 84;
  arr3[0] = 126;
  printf("arr1[0] = %d, arr2[0] = %d, arr3[0] = %d\n", arr1[0], arr2[0], arr3[0]);

  my_free(arr1);
  my_free(arr2);
  my_free(arr3);

  printf("Done!\n");

  return 0;
}
