#ifndef PSRAM_STACK_ALLOCATOR_H
#define PSRAM_STACK_ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>

#if defined(CONFIG_CACHEABLE_PSRAM_TASK_STACK) && CONFIG_CACHEABLE_PSRAM_TASK_STACK

typedef enum {
	PSRAM_STACK_SPAN_INVALID = -1,
	PSRAM_STACK_SPAN_NOT_CACHEABLE = 0,
	PSRAM_STACK_SPAN_VALID = 1,
} psram_stack_span_result_t;

typedef enum {
	PSRAM_STACK_STATUS_OK = 0,
	PSRAM_STACK_STATUS_INVALID = -1,
	PSRAM_STACK_STATUS_CACHE_ERROR = -2,
	PSRAM_STACK_STATUS_GUARD_ERROR = -3,
	PSRAM_STACK_STATUS_NO_MEMORY = -4,
} psram_stack_status_t;

typedef enum {
	PSRAM_STACK_PM_TRANSITION_NONE = 0,
	PSRAM_STACK_PM_TRANSITION_FIRST_LIVE,
	PSRAM_STACK_PM_TRANSITION_LAST_FREE,
} psram_stack_pm_transition_kind_t;

#define PSRAM_STACK_PM_GENERATION_API_VERSION 2U

typedef struct {
	psram_stack_pm_transition_kind_t kind;
	uint64_t generation;
} psram_stack_pm_transition_t;

int psram_stack_alloc_with_transition(
	size_t requested_size,
	void **payload,
	psram_stack_pm_transition_t *transition);
int psram_stack_alloc(
	size_t requested_size,
	void **payload);
psram_stack_span_result_t psram_stack_owner_span(
	const void *pxStack,
	size_t ulStackSize,
	void **span_start,
	size_t *span_size);
int psram_stack_cache_release(
	const void *pxStack,
	size_t ulStackSize);
int psram_stack_cache_acquire(
	const void *pxStack,
	size_t ulStackSize);
int psram_stack_free_exact(
	void *pxStack,
	size_t ulStackSize);
int psram_stack_free_exact_with_transition(
	void *pxStack,
	size_t ulStackSize,
	psram_stack_pm_transition_t *transition);
int psram_stack_pm_hold_release_claim(uint64_t *generation);
int psram_stack_pm_hold_release_commit(uint64_t generation);
int psram_stack_pm_hold_release_abort(uint64_t generation);
int psram_stack_pm_resync_claim(uint64_t *generation);
int psram_stack_pm_resync_commit(uint64_t generation);
int psram_stack_pm_resync_abort(uint64_t generation);

#endif

#endif
