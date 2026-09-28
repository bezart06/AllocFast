#include "allocator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
