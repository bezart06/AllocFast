#include "allocator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#define ENABLE_RED_ZONES 1
#define ARENA_SIZE (64 * 1024)

#if ENABLE_RED_ZONES
#define REDZONE_SIZE 8
#define REDZONE_MAGIC 0xAA
#endif

typedef struct BlockMeta {
  size_t size;
#if ENABLE_RED_ZONES
  size_t exact_size;
#endif
  bool is_free;
  struct BlockMeta *next;
  struct BlockMeta *prev;
  struct BlockMeta *next_free;
  struct BlockMeta *prev_free;
} BlockMeta;

#define META_SIZE sizeof(BlockMeta)

// Global state
static BlockMeta *global_base = NULL;
static BlockMeta *global_tail = NULL;
static BlockMeta *global_free_list = NULL;
static AllocStrategy current_strategy = STRATEGY_FIRST_FIT;

// Segregated List (SLUB-like) Structure
#define NUM_CLASSES 11
static size_t class_sizes[NUM_CLASSES] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
static BlockMeta *segregated_lists[NUM_CLASSES] = {NULL};

// Mutex for thread-safety
static pthread_mutex_t alloc_mutex = PTHREAD_MUTEX_INITIALIZER;

static void add_to_global_free_list(BlockMeta *block) {
    block->next_free = global_free_list;
    block->prev_free = NULL;
    if (global_free_list) {
        global_free_list->prev_free = block;
    }
    global_free_list = block;
}

static void remove_from_global_free_list(BlockMeta *block) {
    if (block->prev_free) {
        block->prev_free->next_free = block->next_free;
    } else {
        global_free_list = block->next_free;
    }
    if (block->next_free) {
        block->next_free->prev_free = block->prev_free;
    }
    block->next_free = NULL;
    block->prev_free = NULL;
}

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
  size_t alloc_size = size + META_SIZE;
  if (alloc_size < ARENA_SIZE) {
      alloc_size = ARENA_SIZE;
  }

  void *request = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (request == MAP_FAILED) return NULL;

  BlockMeta *block = (BlockMeta *)request;
  block->size = alloc_size - META_SIZE;
  block->is_free = true;
  block->next = NULL;
  block->prev = last;
  block->next_free = NULL;
  block->prev_free = NULL;

  if (last) {
    last->next = block;
  }
  if (!global_base) {
      global_base = block;
  }
  global_tail = block;

  if (!global_base) {
    global_base = block;
  }
  global_tail = block;

  return block;
}

static BlockMeta *find_first_fit(size_t size) {
  BlockMeta *current = global_free_list;
  while (current) {
    if (current->size >= size) return current;
    current = current->next_free;
  }
  return NULL;
}

static BlockMeta *find_best_fit(size_t size) {
  BlockMeta *current = global_free_list;
  BlockMeta *best_fit = NULL;

  while (current) {
    if (current->size >= size) {
      if (!best_fit || current->size < best_fit->size) {
        best_fit = current;
      }
    }
    current = current->next_free;
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

  BlockMeta *tail = global_tail;

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
    global_tail = block;

    add_to_segregated_list(block);
  }

  if (last) *last = tail;

  BlockMeta *result = segregated_lists[idx];
  remove_from_segregated_list(result);
  return result;
}

static BlockMeta *split_block(BlockMeta *block, size_t size) {
  if (block->size >= size + META_SIZE + 8) {
    BlockMeta *new_block = (BlockMeta *)((char *)block + META_SIZE + size);

    new_block->size = block->size - size - META_SIZE;
    new_block->is_free = true;
    new_block->next_free = NULL;
    new_block->prev_free = NULL;

    new_block->next = block->next;
    new_block->prev = block;

    if (new_block->next) {
      new_block->next->prev = new_block;
    } else {
        global_tail = new_block;
    }
    block->next = new_block;
    block->size = size;

    return new_block;
  }
  return NULL;
}

static BlockMeta *coalesce_blocks(BlockMeta *block) {
  if (block->next && block->next->is_free) {
    if ((char *)block + META_SIZE + block->size == (char *)block->next) {
      remove_from_global_free_list(block->next);
      block->size += META_SIZE + block->next->size;

      if (block->next == global_tail) {
          global_tail = block;
      }

      block->next = block->next->next;
      if (block->next) {
        block->next->prev = block;
      }
    }
  }

  if (block->prev && block->prev->is_free) {
    if ((char *)block->prev + META_SIZE + block->prev->size == (char *)block) {
      remove_from_global_free_list(block->prev);
      block->prev->size += META_SIZE + block->size;

      if (block == global_tail) {
          global_tail = block->prev;
      }

      block->prev->next = block->next;
      if (block->next) {
        block->next->prev = block->prev;
      }
      block = block->prev;
    }
  }

  return block;
}

void *my_malloc(size_t size) {
  if (size == 0) return NULL;
  pthread_mutex_lock(&alloc_mutex);

  size_t exact_req_size = size;
  size_t alloc_size = size;

#if ENABLE_RED_ZONES
  alloc_size += 2 * REDZONE_SIZE;
#endif
  alloc_size = (alloc_size + 7) & ~7;

  BlockMeta *block = NULL;
  BlockMeta *last = NULL;

  if (current_strategy == STRATEGY_SEGREGATED) {
    block = find_segregated_fit(&last, alloc_size);
    if (!block) {
        block = request_memory(global_tail, alloc_size);
        if (!block) goto fail;
    }
    block->is_free = false;
  } else {
    if (current_strategy == STRATEGY_FIRST_FIT) {
      block = find_first_fit(alloc_size);
    } else if (current_strategy == STRATEGY_BEST_FIT) {
      block = find_best_fit(alloc_size);
    }

    if (!block) {
      block = request_memory(global_tail, alloc_size);
      if (!block) goto fail;
    } else {
      remove_from_global_free_list(block);
    }

    BlockMeta *remainder = split_block(block, alloc_size);
    if (remainder) {
        add_to_global_free_list(remainder);
    }
    block->is_free = false;
  }

#if ENABLE_RED_ZONES
  block->exact_size = exact_req_size;
  char *front_rz = (char *)(block + 1);
  char *payload = front_rz + REDZONE_SIZE;
  char *back_rz = payload + exact_req_size;

  memset(front_rz, REDZONE_MAGIC, REDZONE_SIZE);
  memset(back_rz, REDZONE_MAGIC, REDZONE_SIZE);

  pthread_mutex_unlock(&alloc_mutex);
  return payload;
#else
  pthread_mutex_unlock(&alloc_mutex);
  return (block + 1);
#endif

fail:
  pthread_mutex_unlock(&alloc_mutex);
  return NULL;
}

static BlockMeta *get_block_ptr(void *ptr) {
#if ENABLE_RED_ZONES
  return (BlockMeta *)((char *)ptr - REDZONE_SIZE) - 1;
#else
  return (BlockMeta *)ptr - 1;
#endif
}

void my_free(void *ptr) {
  if (!ptr) return;
  pthread_mutex_lock(&alloc_mutex);

  BlockMeta *block = get_block_ptr(ptr);

#if ENABLE_RED_ZONES
  char *payload = (char *)ptr;
  char *front_rz = (char *)(block + 1);
  char *back_rz = payload + block->exact_size;

  bool corrupted = false;
  for (int i = 0; i < REDZONE_SIZE; i++) {
    if ((unsigned char)front_rz[i] != REDZONE_MAGIC) {
      fprintf(stderr, "\n[FATAL ERROR] Underflow detected at block %p!\n", ptr);
      corrupted = true;
      break;
    }
    if ((unsigned char)back_rz[i] != REDZONE_MAGIC) {
      fprintf(stderr, "\n[FATAL ERROR] Overflow detected at block %p!\n", ptr);
      corrupted = true;
      break;
    }
  }

  if (corrupted) {
    fprintf(stderr, "Aborting execution.\n");
    pthread_mutex_unlock(&alloc_mutex);
    abort();
  }
#endif

  block->is_free = true;

  if (current_strategy == STRATEGY_SEGREGATED) {
      int idx = get_class_index(block->size);
      if (idx != -1) {
          add_to_segregated_list(block);
      } else {
          coalesce_blocks(block);
      }
  } else {
      block = coalesce_blocks(block);
      add_to_global_free_list(block);
  }

  pthread_mutex_unlock(&alloc_mutex);
}

void set_alloc_strategy(AllocStrategy strategy) {
    pthread_mutex_lock(&alloc_mutex);
    current_strategy = strategy;
    pthread_mutex_unlock(&alloc_mutex);
}

// MICRO-BENCHMARK SUITE
#define BENCHMARK_ALLOCS 50000
#define MAX_ALLOC_SIZE 4096

static double get_time_diff(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

static void run_benchmarks(void) {
    void *ptrs[BENCHMARK_ALLOCS];
    size_t sizes[BENCHMARK_ALLOCS];
    struct timespec start, end;

    srand(42);
    for (int i = 0; i < BENCHMARK_ALLOCS; i++) {
        sizes[i] = (rand() % MAX_ALLOC_SIZE) + 1;
    }

    printf("Benchmarking %d allocations & frees (Max size: %d bytes)...\n\n", BENCHMARK_ALLOCS, MAX_ALLOC_SIZE);

    // glibc malloc/free
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < BENCHMARK_ALLOCS; i++) {
        ptrs[i] = malloc(sizes[i]);
    }
    for (int i = 0; i < BENCHMARK_ALLOCS; i++) {
        free(ptrs[i]);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("[1] glibc malloc/free:      %f seconds\n", get_time_diff(start, end));

    // AllocFast - First Fit
    set_alloc_strategy(STRATEGY_FIRST_FIT);
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < BENCHMARK_ALLOCS; i++) {
        ptrs[i] = my_malloc(sizes[i]);
    }
    for (int i = 0; i < BENCHMARK_ALLOCS; i++) {
        my_free(ptrs[i]);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("[2] AllocFast (First Fit):  %f seconds\n", get_time_diff(start, end));

    // AllocFast - Best Fit
    set_alloc_strategy(STRATEGY_BEST_FIT);
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < BENCHMARK_ALLOCS; i++) {
        ptrs[i] = my_malloc(sizes[i]);
    }
    for (int i = 0; i < BENCHMARK_ALLOCS; i++) {
        my_free(ptrs[i]);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("[3] AllocFast (Best Fit):   %f seconds\n", get_time_diff(start, end));

    // AllocFast - Segregated
    set_alloc_strategy(STRATEGY_SEGREGATED);
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < BENCHMARK_ALLOCS; i++) {
        ptrs[i] = my_malloc(sizes[i]);
    }
    for (int i = 0; i < BENCHMARK_ALLOCS; i++) {
        my_free(ptrs[i]);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("[4] AllocFast (Segregated): %f seconds\n\n", get_time_diff(start, end));
}

int main(void) {
  printf("AllocFast Micro-Benchmarks:\n");
  run_benchmarks();

  printf("Testing Normal Allocation:\n");
  int *arr = (int *)my_malloc(100 * sizeof(int));
  arr[0] = 42;
  my_free(arr);
  printf("Normal Allocation works good.\n\n");

  printf("Testing Red Zones (Deliberate Buffer Overflow):\n");
  set_alloc_strategy(STRATEGY_FIRST_FIT);

  char *str = (char *)my_malloc(10); // Requesting exactly 10 bytes

  // We intentionally write 12 bytes here.
  // It will overrun the 10-byte boundary and write into the BACK REDZONE
  strcpy(str, "0123456789A");

  printf("Wrote 12 bytes into a 10-byte buffer...\n");
  printf("Attempting to free buffer (should trigger redzone protection)...\n");

  my_free(str); // Valgrind-like crash occurs here

  printf("Done! (You won't see this if redzones are enabled)\n");

  return 0;
}
