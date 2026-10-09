#ifndef CACHE_PSRAM_STACK_INTERNAL_H
#define CACHE_PSRAM_STACK_INTERNAL_H

#include <stddef.h>

#if defined(CONFIG_CACHEABLE_PSRAM_TASK_STACK) && CONFIG_CACHEABLE_PSRAM_TASK_STACK

typedef enum {
	PSRAM_STACK_OVERLAP_INVALID = -1,
	PSRAM_STACK_OVERLAP_OUTSIDE = 0,
	PSRAM_STACK_OVERLAP = 1,
} psram_stack_overlap_t;

psram_stack_overlap_t psram_stack_heap_overlap(const void *addr, size_t size);
int cache_stack_release_aligned(void *addr, size_t size);
int cache_stack_acquire_aligned(void *addr, size_t size);
int cache_stack_metadata_clean_aligned(void *addr, size_t size);
void cache_stack_metadata_clean_commit(void *addr, size_t size);
void cache_stack_metadata_retire_commit(void *addr, size_t size);

#endif

#endif
