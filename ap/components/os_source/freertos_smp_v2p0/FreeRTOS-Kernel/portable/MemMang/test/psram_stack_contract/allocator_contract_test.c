#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "stubs/common/bk_include.h"
#include "stubs/spinlock.h"
#include "../../psram_stack_allocator.h"

#include "../../heap_4.c"

typedef struct {
	void *addr;
	size_t size;
	size_t requested_size;
} psram_stack_test_span_t;

static int test_psram_stack_span_matches(
	const void *payload,
	const psram_stack_test_span_t *span)
{
	void *span_start;
	size_t span_size;

	return (span != NULL)
		&& (psram_stack_owner_span(
			payload,
			span->requested_size,
			&span_start,
			&span_size) == PSRAM_STACK_SPAN_VALID)
		&& (span_start == span->addr)
		&& (span_size == span->size);
}

static int test_psram_stack_alloc(
	size_t requested_size,
	void **payload,
	psram_stack_test_span_t *span)
{
	if ((span == NULL)
		|| (psram_stack_alloc(requested_size, payload)
			!= PSRAM_STACK_STATUS_OK)) {
		return PSRAM_STACK_STATUS_INVALID;
	}
	span->requested_size = requested_size;
	return (psram_stack_owner_span(
			*payload, requested_size, &span->addr, &span->size)
			== PSRAM_STACK_SPAN_VALID)
		? PSRAM_STACK_STATUS_OK : PSRAM_STACK_STATUS_INVALID;
}

static int test_psram_stack_cache_release(
	const psram_stack_test_span_t *span)
{
	return test_psram_stack_span_matches(span->addr, span)
		? psram_stack_cache_release(span->addr, span->requested_size)
		: PSRAM_STACK_STATUS_INVALID;
}

static int test_psram_stack_cache_acquire(
	const psram_stack_test_span_t *span)
{
	return test_psram_stack_span_matches(span->addr, span)
		? psram_stack_cache_acquire(span->addr, span->requested_size)
		: PSRAM_STACK_STATUS_INVALID;
}

static int test_psram_stack_free_exact(
	void *payload,
	const psram_stack_test_span_t *span)
{
	return test_psram_stack_span_matches(payload, span)
		? psram_stack_free_exact(payload, span->requested_size)
		: PSRAM_STACK_STATUS_INVALID;
}

#if CONFIG_MEM_DEBUG != 1
#error "CACHEALLOC-02 host coverage requires CONFIG_MEM_DEBUG=1"
#endif

#ifndef ITERATIONS
#define ITERATIONS 100000U
#endif

#define HEAP_SIZE 4096U
#define CACHE_LINE_SIZE 32U
#define CACHE_LINE_COUNT (HEAP_SIZE / CACHE_LINE_SIZE)
#define THREAD_SLOTS 16U
#define CHILD_TIMEOUT_SECONDS 1U

typedef struct {
	uint8_t data[CACHE_LINE_SIZE];
	bool valid;
	bool dirty;
} model_cache_line_t;

typedef struct {
	uint8_t physical[HEAP_SIZE];
	model_cache_line_t core[2][CACHE_LINE_COUNT];
} noncoherent_model_t;

typedef struct {
	uint8_t physical[HEAP_SIZE];
	PsramStackBlock_t start;
	PsramStackBlock_t *end;
	size_t free_bytes;
	size_t minimum_free_bytes;
	size_t allocated_bit;
	uint32_t stack_live_count;
	uint32_t shared_used_count;
	uint32_t fixed_used_count;
	uint32_t hold_generation;
	uint32_t next_generation;
	uint32_t provisional_hold;
	uint32_t power_inflight;
} allocator_snapshot_t;

typedef struct {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	unsigned int next_operation;
	unsigned int total_operations;
} deterministic_schedule_t;

typedef struct {
	deterministic_schedule_t *schedule;
	unsigned int core_id;
	unsigned int failures;
	uint32_t random_state;
	void *payloads[THREAD_SLOTS];
	psram_stack_test_span_t spans[THREAD_SLOTS];
} contention_context_t;

_Alignas(32) uint8_t g_psram_stack_test_heap[HEAP_SIZE];
_Alignas(32) uint8_t g_psram_test_ordinary_heap[HEAP_SIZE];
uint32_t g_fixed_psram_power_down;
uint32_t g_fixed_psram_used_count;

static noncoherent_model_t s_model;
static _Thread_local unsigned int s_core_id;
static _Thread_local unsigned int s_lock_depth;
static _Thread_local unsigned int s_kernel_lock_depth;
static bool s_psram_heap_initialized;
static unsigned int s_failure_count;
static unsigned int s_check_count;
static unsigned int s_assert_count;
static unsigned int s_lock_order_violations;
static unsigned int s_pm_failure_count;
static unsigned int s_power_on_call_count;
static unsigned int s_power_on_inside_heap_lock;
static uint32_t s_power_on_shared_count;
static uint32_t s_power_on_fixed_count;
static uint32_t s_power_on_live_count;
static unsigned int s_power_on_dmb_count;
static unsigned int s_power_on_cache_call_count;
static int s_power_on_result;
static pthread_mutex_t s_power_block_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_power_block_condition = PTHREAD_COND_INITIALIZER;
static bool s_power_block_enabled;
static bool s_power_block_entered;
static bool s_power_block_release;
static unsigned int s_heap_enter_count;
static unsigned int s_heap_exit_count;
static unsigned int s_dmb_count;
static bool s_flip_magic_on_next_heap_enter;
static uint32_t s_flip_magic_value;
static unsigned int s_pm_fatal_hook_calls;
static unsigned int s_pm_fatal_hook_lock_depth;
static unsigned int s_extra_lock_calls_in_fatal;
static unsigned int s_cache_call_count;
static unsigned int s_fail_cache_call;

static void fail_case(const char *name)
{
	fprintf(stderr, "FAIL %s\n", name);
	s_failure_count++;
}

static void expect_true(const char *name, bool condition)
{
	s_check_count++;
	if (!condition) {
		fail_case(name);
	}
}

static void expect_success(const char *name, int result)
{
	s_check_count++;
	if (result != 0) {
		fprintf(stderr, "FAIL %s: result=%d\n", name, result);
		s_failure_count++;
	}
}

static void expect_rejected(const char *name, int result)
{
	s_check_count++;
	if (result == 0) {
		fprintf(stderr, "FAIL %s: unexpectedly accepted\n", name);
		s_failure_count++;
	}
}

static int model_range(
	const void *address,
	size_t size,
	size_t *first_line,
	size_t *line_count)
{
	const uintptr_t heap_start = (uintptr_t)g_psram_stack_test_heap;
	const uintptr_t start = (uintptr_t)address;

	if ((address == NULL) || (size == 0U)
		|| (start < heap_start)
		|| (size > HEAP_SIZE)
		|| ((start - heap_start) > HEAP_SIZE - size)
		|| (((start - heap_start) & (CACHE_LINE_SIZE - 1U)) != 0U)
		|| ((size & (CACHE_LINE_SIZE - 1U)) != 0U)) {
		return -1;
	}
	*first_line = (size_t)((start - heap_start) / CACHE_LINE_SIZE);
	*line_count = size / CACHE_LINE_SIZE;
	return 0;
}

static void model_materialize_core(unsigned int core_id)
{
	for (size_t line = 0U; line < CACHE_LINE_COUNT; line++) {
		const uint8_t *source = s_model.core[core_id][line].valid
			? s_model.core[core_id][line].data
			: &s_model.physical[line * CACHE_LINE_SIZE];
		memcpy(
			&g_psram_stack_test_heap[line * CACHE_LINE_SIZE],
			source,
			CACHE_LINE_SIZE);
	}
}

static void model_capture_line(unsigned int core_id, size_t line)
{
	model_cache_line_t *cache_line = &s_model.core[core_id][line];
	uint8_t *view = &g_psram_stack_test_heap[line * CACHE_LINE_SIZE];
	const uint8_t *baseline = cache_line->valid
		? cache_line->data
		: &s_model.physical[line * CACHE_LINE_SIZE];

	if (memcmp(view, baseline, CACHE_LINE_SIZE) != 0) {
		memcpy(cache_line->data, view, CACHE_LINE_SIZE);
		cache_line->valid = true;
		cache_line->dirty = true;
	}
}

static void model_capture_core(unsigned int core_id)
{
	for (size_t line = 0U; line < CACHE_LINE_COUNT; line++) {
		model_capture_line(core_id, line);
	}
}

static bool model_should_fail(void)
{
	s_cache_call_count++;
	return (s_fail_cache_call != 0U)
		&& (s_cache_call_count == s_fail_cache_call);
}

static int model_clean_impl(void *address, size_t size, bool fallible)
{
	size_t first_line;
	size_t line_count;

	if ((model_range(address, size, &first_line, &line_count) != 0)
		|| (fallible && model_should_fail())) {
		return -1;
	}
	for (size_t offset = 0U; offset < line_count; offset++) {
		const size_t line = first_line + offset;
		model_capture_line(s_core_id, line);
		memcpy(
			&s_model.physical[line * CACHE_LINE_SIZE],
			s_model.core[s_core_id][line].data,
			CACHE_LINE_SIZE);
		s_model.core[s_core_id][line].dirty = false;
	}
	return 0;
}

static int model_clean(void *address, size_t size)
{
	return model_clean_impl(address, size, true);
}

static int model_invalidate_impl(void *address, size_t size, bool fallible)
{
	size_t first_line;
	size_t line_count;

	if ((model_range(address, size, &first_line, &line_count) != 0)
		|| (fallible && model_should_fail())) {
		return -1;
	}
	for (size_t offset = 0U; offset < line_count; offset++) {
		const size_t line = first_line + offset;
		model_capture_line(s_core_id, line);
		memcpy(
			s_model.core[s_core_id][line].data,
			&s_model.physical[line * CACHE_LINE_SIZE],
			CACHE_LINE_SIZE);
		memcpy(
			&g_psram_stack_test_heap[line * CACHE_LINE_SIZE],
			&s_model.physical[line * CACHE_LINE_SIZE],
			CACHE_LINE_SIZE);
		s_model.core[s_core_id][line].valid = true;
		s_model.core[s_core_id][line].dirty = false;
	}
	return 0;
}

static int model_invalidate(void *address, size_t size)
{
	return model_invalidate_impl(address, size, true);
}

static void model_evict_line(unsigned int core_id, const void *address)
{
	const uintptr_t offset =
		(uintptr_t)address - (uintptr_t)g_psram_stack_test_heap;
	const size_t line = (size_t)(offset / CACHE_LINE_SIZE);
	model_cache_line_t *cache_line = &s_model.core[core_id][line];

	if (cache_line->valid && cache_line->dirty) {
		memcpy(
			&s_model.physical[line * CACHE_LINE_SIZE],
			cache_line->data,
			CACHE_LINE_SIZE);
		cache_line->dirty = false;
	}
}

static bool model_line_dirty(unsigned int core_id, const void *address)
{
	const uintptr_t offset =
		(uintptr_t)address - (uintptr_t)g_psram_stack_test_heap;
	const size_t line = (size_t)(offset / CACHE_LINE_SIZE);
	return s_model.core[core_id][line].dirty;
}

static void model_quiesce(void)
{
	for (unsigned int core = 0U; core < 2U; core++) {
		for (size_t line = 0U; line < CACHE_LINE_COUNT; line++) {
			if (s_model.core[core][line].valid
				&& s_model.core[core][line].dirty) {
				memcpy(
					&s_model.physical[line * CACHE_LINE_SIZE],
					s_model.core[core][line].data,
					CACHE_LINE_SIZE);
			}
			s_model.core[core][line].valid = false;
			s_model.core[core][line].dirty = false;
		}
	}
	model_materialize_core(0U);
}

static void model_session_begin(unsigned int core_id)
{
	s_core_id = core_id;
	model_materialize_core(core_id);
}

static void model_session_end(void)
{
	model_capture_core(s_core_id);
}

void cachealloc_test_assert(int condition, const char *expression)
{
	if (condition == 0) {
		fprintf(stderr, "ASSERT %s\n", expression);
		s_assert_count++;
	}
}

void pm05a_test_fatal_reset(void)
{
	s_pm_fatal_hook_calls++;
	s_pm_fatal_hook_lock_depth = s_lock_depth;
}

void pm05a_test_dmb(void)
{
	s_dmb_count++;
	__asm__ volatile ("" ::: "memory");
}

void vPortEnterCritical(spinlock_t *lock)
{
	if (s_flip_magic_on_next_heap_enter) {
		FIXED_ADDR_PSRAM_POWER_DOWN = s_flip_magic_value;
		s_flip_magic_on_next_heap_enter = false;
	}
	if (pthread_mutex_lock(&lock->mutex) != 0) {
		s_lock_order_violations++;
		return;
	}
	if ((s_lock_depth != 0U) || (s_kernel_lock_depth != 0U)) {
		s_lock_order_violations++;
	}
	s_lock_depth++;
	s_heap_enter_count++;
	model_materialize_core(s_core_id);
}

void vPortExitCritical(spinlock_t *lock)
{
	if (s_lock_depth == 0U) {
		s_lock_order_violations++;
	} else {
		model_capture_core(s_core_id);
		s_lock_depth--;
		s_heap_exit_count++;
	}
	if (pthread_mutex_unlock(&lock->mutex) != 0) {
		s_lock_order_violations++;
	}
}

int cache_stack_release_aligned(void *address, size_t size)
{
	if (model_clean(address, size) != 0) {
		return -1;
	}
	return model_invalidate(address, size);
}

int cache_stack_acquire_aligned(void *address, size_t size)
{
	return model_invalidate(address, size);
}

int cache_stack_metadata_clean_aligned(void *address, size_t size)
{
	return model_clean(address, size);
}

void cache_stack_metadata_clean_commit(void *address, size_t size)
{
	if (model_clean_impl(address, size, false) != 0) {
		fail_case("metadata clean commit range");
	}
}

void cache_stack_metadata_retire_commit(void *address, size_t size)
{
	if (model_invalidate_impl(address, size, false) != 0) {
		fail_case("metadata retire commit range");
	}
}

psram_stack_overlap_t psram_stack_heap_overlap(const void *address, size_t size)
{
	const uintptr_t start = (uintptr_t)address;
	const uintptr_t heap_start = (uintptr_t)g_psram_stack_test_heap;
	const uintptr_t heap_end = heap_start + HEAP_SIZE;

	if ((address == NULL) || (size == 0U) || (size > UINTPTR_MAX - start)) {
		return PSRAM_STACK_OVERLAP_INVALID;
	}
	if ((start + size <= heap_start) || (start >= heap_end)) {
		return PSRAM_STACK_OVERLAP_OUTSIDE;
	}
	return PSRAM_STACK_OVERLAP;
}

bool bk_psram_heap_init_flag_get(void)
{
	return s_psram_heap_initialized;
}

void bk_psram_heap_init_flag_set(bool initialized)
{
	s_psram_heap_initialized = initialized;
}

int bk_pm_module_vote_psram_ctrl(int module, int state)
{
	(void)module;
	(void)state;
	s_power_on_call_count++;
	s_power_on_shared_count = s_psram_used_count;
	s_power_on_fixed_count = FIXED_ADDR_PSRAM_USDE_COUNT;
	s_power_on_live_count = s_psram_stack_live_count;
	s_power_on_dmb_count = s_dmb_count;
	s_power_on_cache_call_count = s_cache_call_count;
	if (s_lock_depth != 0U) {
		s_power_on_inside_heap_lock++;
	}
	if (s_power_block_enabled && (s_power_on_call_count == 1U)) {
		(void)pthread_mutex_lock(&s_power_block_mutex);
		s_power_block_entered = true;
		(void)pthread_cond_broadcast(&s_power_block_condition);
		while (!s_power_block_release) {
			(void)pthread_cond_wait(
				&s_power_block_condition, &s_power_block_mutex);
		}
		(void)pthread_mutex_unlock(&s_power_block_mutex);
	}
	return s_power_on_result;
}

UBaseType_t xTaskGetTickCount(void)
{
	return 0U;
}

const char *pcTaskGetName(TaskHandle_t task)
{
	(void)task;
	return "cachealloc-test";
}

int rtos_is_scheduler_started(void)
{
	return 0;
}

int platform_is_in_interrupt_context(void)
{
	return 0;
}

int arch_is_enter_exception(void)
{
	return 0;
}

void rtos_regist_plat_dump_hook(uint32_t address, uint32_t size)
{
	(void)address;
	(void)size;
}

void stack_mem_dump(uint32_t start, uint32_t end)
{
	(void)start;
	(void)end;
}

static void allocator_reset(void)
{
	model_quiesce();
	memset(&s_model, 0, sizeof(s_model));
	memset(g_psram_stack_test_heap, 0, sizeof(g_psram_stack_test_heap));
	memset(g_psram_test_ordinary_heap, 0, sizeof(g_psram_test_ordinary_heap));
	memset(&s_psram_stack_start, 0, sizeof(s_psram_stack_start));
	s_psram_stack_end = NULL;
	s_psram_stack_free_bytes = 0U;
	s_psram_stack_minimum_ever_free_bytes = 0U;
	s_psram_stack_live_count = 0U;
	s_psram_stack_pm_hold_generation = 0U;
	s_psram_stack_pm_generation = 0U;
	s_psram_stack_pm_provisional_hold = 0U;
	s_psram_power_inflight = 0U;
	s_psram_used_count = 0U;
	FIXED_ADDR_PSRAM_USDE_COUNT = 0U;
	FIXED_ADDR_PSRAM_POWER_DOWN = 0U;
	xBlockAllocatedBit = 0U;
	s_core_id = 0U;
	s_lock_depth = 0U;
	s_kernel_lock_depth = 0U;
	s_cache_call_count = 0U;
	s_fail_cache_call = 0U;
	s_power_on_call_count = 0U;
	s_power_on_inside_heap_lock = 0U;
	s_power_on_shared_count = 0U;
	s_power_on_fixed_count = 0U;
	s_power_on_live_count = 0U;
	s_power_on_dmb_count = 0U;
	s_power_on_cache_call_count = 0U;
	s_power_on_result = BK_OK;
	s_power_block_enabled = false;
	s_power_block_entered = false;
	s_power_block_release = false;
	s_heap_enter_count = 0U;
	s_heap_exit_count = 0U;
	s_dmb_count = 0U;
	s_flip_magic_on_next_heap_enter = false;
	s_flip_magic_value = 0U;
	s_pm_fatal_hook_calls = 0U;
	s_pm_fatal_hook_lock_depth = 0U;
	s_extra_lock_calls_in_fatal = 0U;
	model_materialize_core(0U);
}

static void snapshot_take(allocator_snapshot_t *snapshot)
{
	model_quiesce();
	memcpy(snapshot->physical, s_model.physical, HEAP_SIZE);
	snapshot->start = s_psram_stack_start;
	snapshot->end = s_psram_stack_end;
	snapshot->free_bytes = s_psram_stack_free_bytes;
	snapshot->minimum_free_bytes =
		s_psram_stack_minimum_ever_free_bytes;
	snapshot->allocated_bit = xBlockAllocatedBit;
	snapshot->stack_live_count = s_psram_stack_live_count;
	snapshot->shared_used_count = s_psram_used_count;
	snapshot->fixed_used_count = FIXED_ADDR_PSRAM_USDE_COUNT;
	snapshot->hold_generation = s_psram_stack_pm_hold_generation;
	snapshot->next_generation = s_psram_stack_pm_generation;
	snapshot->provisional_hold = s_psram_stack_pm_provisional_hold;
	snapshot->power_inflight = s_psram_power_inflight;
}

static void snapshot_restore(const allocator_snapshot_t *snapshot)
{
	memset(&s_model, 0, sizeof(s_model));
	memcpy(s_model.physical, snapshot->physical, HEAP_SIZE);
	s_psram_stack_start = snapshot->start;
	s_psram_stack_end = snapshot->end;
	s_psram_stack_free_bytes = snapshot->free_bytes;
	s_psram_stack_minimum_ever_free_bytes =
		snapshot->minimum_free_bytes;
	xBlockAllocatedBit = snapshot->allocated_bit;
	s_psram_stack_live_count = snapshot->stack_live_count;
	s_psram_used_count = snapshot->shared_used_count;
	FIXED_ADDR_PSRAM_USDE_COUNT = snapshot->fixed_used_count;
	s_psram_stack_pm_hold_generation = snapshot->hold_generation;
	s_psram_stack_pm_generation = snapshot->next_generation;
	s_psram_stack_pm_provisional_hold = snapshot->provisional_hold;
	s_psram_power_inflight = snapshot->power_inflight;
	s_cache_call_count = 0U;
	s_fail_cache_call = 0U;
	model_materialize_core(0U);
}

static void snapshot_expect_equal(
	const char *name,
	const allocator_snapshot_t *expected)
{
	char check_name[96];

	model_quiesce();
	snprintf(check_name, sizeof(check_name), "%s metadata", name);
	expect_true(
		check_name,
		memcmp(expected->physical, s_model.physical, HEAP_SIZE) == 0);
	snprintf(check_name, sizeof(check_name), "%s free-list head", name);
	expect_true(
		check_name,
		memcmp(
			&expected->start,
			&s_psram_stack_start,
			sizeof(expected->start)) == 0);
	snprintf(check_name, sizeof(check_name), "%s sentinel", name);
	expect_true(check_name, expected->end == s_psram_stack_end);
	snprintf(check_name, sizeof(check_name), "%s free-byte count", name);
	expect_true(check_name, expected->free_bytes == s_psram_stack_free_bytes);
	snprintf(check_name, sizeof(check_name), "%s minimum-free count", name);
	expect_true(
		check_name,
		expected->minimum_free_bytes
			== s_psram_stack_minimum_ever_free_bytes);
	snprintf(check_name, sizeof(check_name), "%s allocation bit", name);
	expect_true(check_name, expected->allocated_bit == xBlockAllocatedBit);
	snprintf(check_name, sizeof(check_name), "%s stack-live count", name);
	expect_true(
		check_name,
		expected->stack_live_count == s_psram_stack_live_count);
	snprintf(check_name, sizeof(check_name), "%s shared-used count", name);
	expect_true(
		check_name,
		expected->shared_used_count == s_psram_used_count);
	snprintf(check_name, sizeof(check_name), "%s fixed-used count", name);
	expect_true(
		check_name,
		expected->fixed_used_count == FIXED_ADDR_PSRAM_USDE_COUNT);
	snprintf(check_name, sizeof(check_name), "%s PM hold", name);
	expect_true(
		check_name,
		expected->hold_generation
			== s_psram_stack_pm_hold_generation);
	snprintf(check_name, sizeof(check_name), "%s PM generation", name);
	expect_true(
		check_name,
		expected->next_generation == s_psram_stack_pm_generation);
	snprintf(check_name, sizeof(check_name), "%s provisional hold", name);
	expect_true(
		check_name,
		expected->provisional_hold
			== s_psram_stack_pm_provisional_hold);
}

static size_t padded_payload_size(size_t requested_size)
{
	return (requested_size + CACHE_LINE_SIZE - 1U)
		& ~((size_t)CACHE_LINE_SIZE - 1U);
}

static void prepare_free_heap(allocator_snapshot_t *snapshot)
{
	void *payload = NULL;
	psram_stack_test_span_t span = {0};

	allocator_reset();
	expect_success("baseline alloc", test_psram_stack_alloc(32U, &payload, &span));
	expect_success("baseline free", test_psram_stack_free_exact(payload, &span));
	/* ADR-004: the last free retains one PM hold and a pending hold rejects
	 * further allocation, so complete the release round trip to reach the
	 * quiescent free heap these atomicity cases snapshot. */
	expect_success(
		"baseline hold release",
		psram_stack_pm_hold_release_commit(
			s_psram_stack_pm_hold_generation));
	snapshot_take(snapshot);
}

static void test_allocation_failure_atomicity(void)
{
	allocator_snapshot_t baseline;
	void *payload = NULL;
	psram_stack_test_span_t span = {0};

	prepare_free_heap(&baseline);
	snapshot_restore(&baseline);
	s_cache_call_count = 0U;
	expect_success("reference allocation", test_psram_stack_alloc(33U, &payload, &span));
	const unsigned int fallible_calls = s_cache_call_count;
	expect_true("allocation has fallible cache points", fallible_calls > 0U);

	unsigned int fail_stop_points = 0U;
	for (unsigned int point = 1U; point <= fallible_calls; point++) {
		char name[80];
		snapshot_restore(&baseline);
		s_pm_fatal_hook_calls = 0U;
		s_fail_cache_call = point;
		const int result = test_psram_stack_alloc(33U, &payload, &span);
		snprintf(name, sizeof(name), "alloc failure atomic point=%u", point);
		expect_rejected(name, result);
		s_fail_cache_call = 0U;
		/* PM-05A routes authoritative metadata/topology cache failure to the
		 * crash-safe fail-stop seam, where the device reboots instead of
		 * unwinding. Only gracefully rejected points owe an atomic rollback. */
		if (s_pm_fatal_hook_calls == 0U) {
			snapshot_expect_equal(name, &baseline);
		} else {
			fail_stop_points++;
		}
	}
	expect_true(
		"allocation failure points are rejected or fail-stop",
		fail_stop_points <= fallible_calls);
	s_pm_fatal_hook_calls = 0U;
}

static void test_free_failure_atomicity(void)
{
	allocator_snapshot_t allocated;
	void *payload = NULL;
	psram_stack_test_span_t span = {0};

	allocator_reset();
	expect_success("free baseline alloc", test_psram_stack_alloc(64U, &payload, &span));
	snapshot_take(&allocated);
	snapshot_restore(&allocated);
	s_cache_call_count = 0U;
	expect_success("reference free", test_psram_stack_free_exact(payload, &span));
	const unsigned int fallible_calls = s_cache_call_count;
	expect_true("free has fallible cache points", fallible_calls > 0U);

	for (unsigned int point = 1U; point <= fallible_calls; point++) {
		char name[80];
		snapshot_restore(&allocated);
		s_fail_cache_call = point;
		const int result = test_psram_stack_free_exact(payload, &span);
		snprintf(name, sizeof(name), "free failure atomic point=%u", point);
		expect_rejected(name, result);
		s_fail_cache_call = 0U;
		snapshot_expect_equal(name, &allocated);
	}
}

static void test_forward_merge_retired_line(void)
{
	void *payload[3] = {0};
	psram_stack_test_span_t span[3] = {0};

	allocator_reset();
	s_core_id = 0U;
	for (size_t index = 0U; index < 3U; index++) {
		expect_success(
			"forward setup alloc",
			test_psram_stack_alloc(32U, &payload[index], &span[index]));
	}
	const size_t header_size =
		(size_t)((uintptr_t)payload[0] - (uintptr_t)g_psram_stack_test_heap);
	const size_t stride =
		(size_t)((uintptr_t)payload[1] - (uintptr_t)payload[0]);
	void *retired_header = (uint8_t *)payload[1] - header_size;

	expect_success("forward free first", test_psram_stack_free_exact(payload[0], &span[0]));
	expect_success("forward free second", test_psram_stack_free_exact(payload[1], &span[1]));
	expect_true(
		"forward merge leaves no dirty retired line",
		!model_line_dirty(0U, retired_header));

	s_core_id = 1U;
	void *merged_payload = NULL;
	psram_stack_test_span_t merged_span = {0};
	const size_t merged_request =
		(2U * stride) - header_size - PSRAM_STACK_GUARD_SIZE;
	const size_t merged_payload_size = padded_payload_size(merged_request);
	expect_success(
		"cross-core merged allocation",
		test_psram_stack_alloc(merged_request, &merged_payload, &merged_span));
	expect_true(
		"retired header reused as payload",
		((uintptr_t)retired_header >= (uintptr_t)merged_payload)
		&& ((uintptr_t)retired_header + CACHE_LINE_SIZE
			<= (uintptr_t)merged_payload + merged_payload_size));

	model_session_begin(1U);
	memset(merged_payload, 0x5a, merged_payload_size);
	expect_success("publish merged payload", test_psram_stack_cache_release(&merged_span));
	model_session_end();
	model_evict_line(0U, retired_header);
	model_session_begin(1U);
	expect_success("reacquire merged payload", test_psram_stack_cache_acquire(&merged_span));
	for (size_t index = 0U; index < merged_payload_size; index++) {
		if (((uint8_t *)merged_payload)[index] != UINT8_C(0x5a)) {
			fail_case("retired dirty metadata overwrote new allocation");
			break;
		}
	}
	model_session_end();
}

static void test_guard_owner_span(void)
{
	void *payload = NULL;
	psram_stack_test_span_t span = {0};
	const size_t requested_size = 33U;
	const size_t payload_size = padded_payload_size(requested_size);

	allocator_reset();
	s_core_id = 0U;
	expect_success(
		"guard owner alloc",
		test_psram_stack_alloc(requested_size, &payload, &span));
	expect_true("allocation span starts at payload", span.addr == payload);
	expect_true(
		"allocation span includes guard",
		span.size == payload_size + PSRAM_STACK_GUARD_SIZE);
	uint8_t *guard = (uint8_t *)payload + payload_size;
	expect_true(
		"guard is final allocation-span line",
		guard + PSRAM_STACK_GUARD_SIZE
			== (uint8_t *)span.addr + span.size);
	model_session_begin(0U);
	guard[0] ^= UINT8_C(0x5a);
	expect_success("owner release", test_psram_stack_cache_release(&span));
	model_session_end();
	expect_true("owner release publishes guard", !model_line_dirty(0U, guard));

	s_core_id = 1U;
	expect_rejected(
		"cross-core free detects published guard corruption",
		test_psram_stack_free_exact(payload, &span));
}

static psram_stack_span_result_t classify_span(
	uintptr_t stack_start, size_t requested_size)
{
	void *span_start = NULL;
	size_t span_size = 0U;

	return psram_stack_owner_span(
		(const void *)stack_start, requested_size, &span_start, &span_size);
}

static void test_owner_span_adjacent_region_boundary(void)
{
	const uintptr_t heap_start = (uintptr_t)g_psram_stack_test_heap;
	const uintptr_t heap_end = heap_start + HEAP_SIZE;
	const size_t stack_size = 2U * CACHE_LINE_SIZE;

	allocator_reset();
	expect_true(
		"stack ending exactly at heap base is not cacheable",
		classify_span(heap_start - stack_size, stack_size)
			== PSRAM_STACK_SPAN_NOT_CACHEABLE);
	expect_true(
		"stack one line below heap base is not cacheable",
		classify_span(heap_start - stack_size - CACHE_LINE_SIZE, stack_size)
			== PSRAM_STACK_SPAN_NOT_CACHEABLE);
	expect_true(
		"stack straddling heap base is rejected",
		classify_span(heap_start - CACHE_LINE_SIZE, stack_size)
			== PSRAM_STACK_SPAN_INVALID);
	expect_true(
		"stack starting at heap limit is not cacheable",
		classify_span(heap_end, stack_size)
			== PSRAM_STACK_SPAN_NOT_CACHEABLE);
	expect_true(
		"in-heap stack leaving no room for its guard is rejected",
		classify_span(heap_end - stack_size, stack_size)
			== PSRAM_STACK_SPAN_INVALID);
	expect_true(
		"in-heap stack with room for its guard is valid",
		classify_span(heap_end - stack_size - CACHE_LINE_SIZE, stack_size)
			== PSRAM_STACK_SPAN_VALID);
}

static void test_free_paths_fail_closed(void)
{
	allocator_snapshot_t allocated;
	allocator_snapshot_t freed;
	void *payload = NULL;
	psram_stack_test_span_t span = {0};
	psram_stack_test_span_t forged;
	const size_t payload_size = padded_payload_size(33U);

	allocator_reset();
	expect_success(
		"fail-closed setup alloc",
		test_psram_stack_alloc(33U, &payload, &span));
	snapshot_take(&allocated);

	vPortFree(payload);
	snapshot_expect_equal("generic free live stack", &allocated);
	expect_rejected(
		"exact free interior payload",
		test_psram_stack_free_exact((uint8_t *)payload + 1U, &span));
	snapshot_expect_equal("interior free unchanged", &allocated);
	expect_rejected(
		"exact free header",
		test_psram_stack_free_exact(
			(uint8_t *)payload - PSRAM_STACK_HEADER_SIZE, &span));
	snapshot_expect_equal("header free unchanged", &allocated);
	expect_rejected(
		"exact free guard",
		test_psram_stack_free_exact(
			(uint8_t *)payload + payload_size, &span));
	snapshot_expect_equal("guard free unchanged", &allocated);
	expect_rejected(
		"exact free cross heap",
		test_psram_stack_free_exact(g_psram_test_ordinary_heap, &span));
	snapshot_expect_equal("cross-heap free unchanged", &allocated);

	forged = span;
	forged.size -= PSRAM_STACK_GUARD_SIZE;
	expect_rejected(
		"exact free forged live span",
		test_psram_stack_free_exact(payload, &forged));
	snapshot_expect_equal("forged span free unchanged", &allocated);

	expect_success("exact free valid span", test_psram_stack_free_exact(payload, &span));
	snapshot_take(&freed);
	expect_rejected(
		"exact free double free",
		test_psram_stack_free_exact(payload, &span));
	snapshot_expect_equal("double free unchanged", &freed);
}

static bool topology_valid(void)
{
	const uintptr_t heap_start = (uintptr_t)g_psram_stack_test_heap;
	const uintptr_t heap_end = heap_start + HEAP_SIZE;
	size_t free_sum = 0U;
	size_t nodes = 0U;
	BlockLink_t *link;
	uintptr_t sentinel;

	if (s_psram_stack_end == NULL) {
		return false;
	}
	sentinel = (uintptr_t)s_psram_stack_end;
	link = s_psram_stack_start.block.pxNextFreeBlock;
	if ((sentinel != heap_end - PSRAM_STACK_HEADER_SIZE)
		|| ((sentinel & (CACHE_LINE_SIZE - 1U)) != 0U)
		|| (link == NULL)
		|| (s_psram_stack_end->block.xBlockSize != 0U)
		|| (s_psram_stack_end->block.pxNextFreeBlock != NULL)) {
		return false;
	}
	while (link != &s_psram_stack_end->block) {
		PsramStackBlock_t *block = (PsramStackBlock_t *)link;
		const uintptr_t start = (uintptr_t)block;
		const size_t size = block->block.xBlockSize;
		BlockLink_t *next = block->block.pxNextFreeBlock;
		const uintptr_t next_start = (uintptr_t)next;
		if (++nodes > CACHE_LINE_COUNT
			|| (start < heap_start) || (start >= sentinel)
			|| ((start & (CACHE_LINE_SIZE - 1U)) != 0U)
			|| !psram_stack_block_size_valid(block, size, sentinel)
			|| (next == NULL)
			|| (next_start <= start)
			|| (next_start > sentinel)
			|| ((next_start & (CACHE_LINE_SIZE - 1U)) != 0U)
			|| ((start + size == next_start)
				&& (next != &s_psram_stack_end->block))
			|| (size > SIZE_MAX - free_sum)) {
			return false;
		}
		free_sum += size;
		link = next;
	}
	return free_sum == s_psram_stack_free_bytes;
}

static void test_link_and_sentinel_validation(void)
{
	allocator_snapshot_t baseline;
	void *payload = NULL;
	psram_stack_test_span_t span = {0};
	PsramStackBlock_t *first;

	prepare_free_heap(&baseline);
	snapshot_restore(&baseline);
	first = (PsramStackBlock_t *)g_psram_stack_test_heap;
	first->block.xBlockSize = HEAP_SIZE;
	expect_success(
		"publish sentinel-overlap metadata",
		cache_stack_metadata_clean_aligned(first, PSRAM_STACK_HEADER_SIZE));
	expect_rejected(
		"sentinel-overlap block",
		test_psram_stack_alloc(32U, &payload, &span));

	snapshot_restore(&baseline);
	first = (PsramStackBlock_t *)g_psram_stack_test_heap;
	first->block.pxNextFreeBlock =
		(BlockLink_t *)((uint8_t *)s_psram_stack_end + 1U);
	expect_success(
		"publish unaligned successor",
		cache_stack_metadata_clean_aligned(first, PSRAM_STACK_HEADER_SIZE));
	expect_rejected(
		"selected block rejects unaligned successor",
		test_psram_stack_alloc(32U, &payload, &span));

	snapshot_restore(&baseline);
	first = (PsramStackBlock_t *)g_psram_stack_test_heap;
	first->block.pxNextFreeBlock =
		(BlockLink_t *)((uintptr_t)g_psram_stack_test_heap
			+ HEAP_SIZE + CACHE_LINE_SIZE);
	expect_success(
		"publish out-of-range successor",
		cache_stack_metadata_clean_aligned(first, PSRAM_STACK_HEADER_SIZE));
	expect_rejected(
		"selected block rejects out-of-range successor",
		test_psram_stack_alloc(32U, &payload, &span));

	snapshot_restore(&baseline);
	s_psram_stack_end->block.xBlockSize = PSRAM_STACK_MINIMUM_BLOCK_SIZE;
	expect_success(
		"publish nonzero sentinel size",
		cache_stack_metadata_clean_aligned(
			s_psram_stack_end, PSRAM_STACK_HEADER_SIZE));
	expect_rejected(
		"nonzero sentinel size",
		test_psram_stack_alloc(32U, &payload, &span));

	snapshot_restore(&baseline);
	first = (PsramStackBlock_t *)g_psram_stack_test_heap;
	s_psram_stack_end->block.pxNextFreeBlock = &first->block;
	expect_success(
		"publish linked sentinel",
		cache_stack_metadata_clean_aligned(
			s_psram_stack_end, PSRAM_STACK_HEADER_SIZE));
	expect_rejected(
		"sentinel next must be null",
		test_psram_stack_alloc(32U, &payload, &span));
}

static void test_free_list_node_validation(void)
{
	void *payload[3] = {0};
	psram_stack_test_span_t span[3] = {0};

	allocator_reset();
	for (size_t index = 0U; index < 3U; index++) {
		expect_success(
			"free-list validation setup alloc",
			test_psram_stack_alloc(32U, &payload[index], &span[index]));
	}
	expect_success(
		"free-list validation setup free",
		test_psram_stack_free_exact(payload[2], &span[2]));
	PsramStackBlock_t *free_node =
		(PsramStackBlock_t *)((uint8_t *)payload[2] - PSRAM_STACK_HEADER_SIZE);
	free_node->block.xBlockSize |= 1U;
	expect_success(
		"publish misaligned free-list node",
		cache_stack_metadata_clean_aligned(
			free_node, PSRAM_STACK_HEADER_SIZE));
	expect_rejected(
		"misaligned free-list node",
		test_psram_stack_free_exact(payload[1], &span[1]));
}

static void test_unsplittable_tail_validation(void)
{
	void *payload = NULL;
	psram_stack_test_span_t span = {0};
	const size_t request_with_legal_tail =
		HEAP_SIZE - (2U * PSRAM_STACK_HEADER_SIZE)
		- PSRAM_STACK_GUARD_SIZE - CACHE_LINE_SIZE;

	allocator_reset();
	expect_success(
		"legal unsplittable tail alloc",
		test_psram_stack_alloc(request_with_legal_tail, &payload, &span));
	PsramStackBlock_t *block =
		(PsramStackBlock_t *)((uint8_t *)payload - PSRAM_STACK_HEADER_SIZE);
	const size_t block_size = block->block.xBlockSize & ~xBlockAllocatedBit;
	const size_t required_size =
		PSRAM_STACK_HEADER_SIZE
		+ padded_payload_size(request_with_legal_tail)
		+ PSRAM_STACK_GUARD_SIZE;
	expect_true(
		"legal unsplittable tail remains below split minimum",
		(block_size >= required_size)
		&& ((block_size - required_size) < PSRAM_STACK_MINIMUM_BLOCK_SIZE));
	expect_success(
		"legal unsplittable tail free",
		test_psram_stack_free_exact(payload, &span));
	expect_true("legal tail cleanup topology", topology_valid());

	allocator_reset();
	expect_success("illegal tail setup alloc", test_psram_stack_alloc(33U, &payload, &span));
	block =
		(PsramStackBlock_t *)((uint8_t *)payload - PSRAM_STACK_HEADER_SIZE);
	block->block.xBlockSize =
		(block->block.xBlockSize & xBlockAllocatedBit)
		| ((block->block.xBlockSize & ~xBlockAllocatedBit)
			+ PSRAM_STACK_MINIMUM_BLOCK_SIZE);
	expect_success(
		"publish illegal tail",
		cache_stack_metadata_clean_aligned(block, PSRAM_STACK_HEADER_SIZE));
	expect_rejected(
		"tail at split minimum must be rejected",
		test_psram_stack_free_exact(payload, &span));
}

static int cycle_child_case(void)
{
	allocator_snapshot_t baseline;
	void *payload = NULL;
	psram_stack_test_span_t span = {0};

	prepare_free_heap(&baseline);
	snapshot_restore(&baseline);
	PsramStackBlock_t *first =
		(PsramStackBlock_t *)g_psram_stack_test_heap;
	first->block.xBlockSize = PSRAM_STACK_MINIMUM_BLOCK_SIZE;
	first->block.pxNextFreeBlock = &first->block;
	if (cache_stack_metadata_clean_aligned(
			first, PSRAM_STACK_HEADER_SIZE) != 0) {
		return 2;
	}
	alarm(CHILD_TIMEOUT_SECONDS);
	return test_psram_stack_alloc(
		PSRAM_STACK_MINIMUM_BLOCK_SIZE + CACHE_LINE_SIZE,
		&payload,
		&span) == 0;
}

static void test_cycle_rejected_without_hang(void)
{
	const pid_t child = fork();
	int status = 0;

	if (child == 0) {
		_exit(cycle_child_case());
	}
	if ((child < 0) || (waitpid(child, &status, 0) != child)) {
		fail_case("cycle child process");
		return;
	}
	expect_true(
		"free-list cycle rejected without hang",
		WIFEXITED(status) && (WEXITSTATUS(status) == 0));
}

static uint32_t next_random(uint32_t *state)
{
	uint32_t value = *state;

	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	*state = value;
	return value;
}

static void *contention_worker(void *argument)
{
	static const size_t sizes[] = {1U, 31U, 32U, 33U, 64U, 95U};
	contention_context_t *context = argument;
	deterministic_schedule_t *schedule = context->schedule;

	s_core_id = context->core_id;
	for (;;) {
		if (pthread_mutex_lock(&schedule->mutex) != 0) {
			context->failures++;
			return NULL;
		}
		while ((schedule->next_operation < schedule->total_operations)
			&& ((schedule->next_operation & 1U) != context->core_id)) {
			if (pthread_cond_wait(
					&schedule->condition, &schedule->mutex) != 0) {
				context->failures++;
				(void)pthread_mutex_unlock(&schedule->mutex);
				return NULL;
			}
		}
		if (schedule->next_operation >= schedule->total_operations) {
			(void)pthread_cond_broadcast(&schedule->condition);
			(void)pthread_mutex_unlock(&schedule->mutex);
			break;
		}
		const uint32_t value = next_random(&context->random_state);
		const size_t slot = value % THREAD_SLOTS;
		if (context->payloads[slot] != NULL) {
			if (test_psram_stack_free_exact(
					context->payloads[slot],
					&context->spans[slot]) != 0) {
				context->failures++;
			}
			context->payloads[slot] = NULL;
		} else {
			const size_t size = sizes[(value >> 8)
				% (sizeof(sizes) / sizeof(sizes[0]))];
			if (test_psram_stack_alloc(
					size,
					&context->payloads[slot],
					&context->spans[slot]) != 0) {
				context->payloads[slot] = NULL;
			}
		}
		schedule->next_operation++;
		if ((pthread_cond_broadcast(&schedule->condition) != 0)
			|| (pthread_mutex_unlock(&schedule->mutex) != 0)) {
			context->failures++;
			return NULL;
		}
	}
	return NULL;
}

static void test_deterministic_two_thread_visibility(void)
{
	pthread_t threads[2];
	deterministic_schedule_t schedule = {
		PTHREAD_MUTEX_INITIALIZER,
		PTHREAD_COND_INITIALIZER,
		0U,
		ITERATIONS,
	};
	contention_context_t contexts[2] = {
		{
			.schedule = &schedule,
			.core_id = 0U,
			.random_state = UINT32_C(0x7258c001),
		},
		{
			.schedule = &schedule,
			.core_id = 1U,
			.random_state = UINT32_C(0x7258c002),
		},
	};

	allocator_reset();
	for (size_t index = 0U; index < 2U; index++) {
		if (pthread_create(
				&threads[index], NULL, contention_worker, &contexts[index]) != 0) {
			fail_case("pthread_create");
			return;
		}
	}
	for (size_t index = 0U; index < 2U; index++) {
		if (pthread_join(threads[index], NULL) != 0) {
			fail_case("pthread_join");
		}
	}
	expect_true(
		"deterministic operation schedule complete",
		schedule.next_operation == schedule.total_operations);
	for (size_t core = 0U; core < 2U; core++) {
		s_core_id = (unsigned int)core;
		for (size_t slot = 0U; slot < THREAD_SLOTS; slot++) {
			if ((contexts[core].payloads[slot] != NULL)
				&& (test_psram_stack_free_exact(
					contexts[core].payloads[slot],
					&contexts[core].spans[slot]) != 0)) {
				contexts[core].failures++;
			}
		}
	}
	model_quiesce();
	expect_true(
		"deterministic two-thread operations complete",
		(contexts[0].failures == 0U) && (contexts[1].failures == 0U));
	expect_true("deterministic two-view heap topology", topology_valid());
	expect_true(
		"deterministic two-view full cleanup",
		s_psram_stack_free_bytes
			== HEAP_SIZE - PSRAM_STACK_HEADER_SIZE);
	(void)pthread_cond_destroy(&schedule.condition);
	(void)pthread_mutex_destroy(&schedule.mutex);
}

static void test_pm_shared_accounting_transitions(void)
{
	void *payload[2] = {0};
	psram_stack_test_span_t span[2] = {0};
	const unsigned int failures_before = s_failure_count;

	allocator_reset();
	expect_success(
		"PM-05A first committed stack allocation",
		test_psram_stack_alloc(32U, &payload[0], &span[0]));
	expect_true(
		"PM-05A first commit increments shared used count",
		s_psram_used_count == 1U);
	expect_true(
		"PM-05A first commit increments fixed used count",
		FIXED_ADDR_PSRAM_USDE_COUNT == 1U);

	expect_success(
		"PM-05A second committed stack allocation",
		test_psram_stack_alloc(64U, &payload[1], &span[1]));
	expect_true(
		"PM-05A second commit increments shared used count exactly",
		s_psram_used_count == 2U);
	expect_true(
		"PM-05A second commit increments fixed used count exactly",
		FIXED_ADDR_PSRAM_USDE_COUNT == 2U);

	expect_success(
		"PM-05A non-final exact free",
		test_psram_stack_free_exact(payload[0], &span[0]));
	expect_true(
		"PM-05A non-final free decrements shared used count",
		s_psram_used_count == 1U);
	expect_true(
		"PM-05A non-final free decrements fixed used count",
		FIXED_ADDR_PSRAM_USDE_COUNT == 1U);

	expect_success(
		"PM-05A final exact free",
		test_psram_stack_free_exact(payload[1], &span[1]));
	expect_true(
		"PM-05A final free retains one shared PM hold",
		s_psram_used_count == 1U);
	expect_true(
		"PM-05A final free retains one fixed PM hold",
		FIXED_ADDR_PSRAM_USDE_COUNT == 1U);

	if (s_failure_count != failures_before) {
		s_pm_failure_count += s_failure_count - failures_before;
		fprintf(
			stderr,
			"PM-05A:ACCOUNTING:FAIL shared=%u fixed=%u\n",
			(unsigned int)s_psram_used_count,
			(unsigned int)FIXED_ADDR_PSRAM_USDE_COUNT);
	} else {
		printf("PM-05A:ACCOUNTING:PASS shared=1 fixed=1 hold=1\n");
	}
}

static int report_result(void)
{
	if ((s_pm_failure_count != 0U)
		&& (s_failure_count == s_pm_failure_count)
		&& (s_assert_count == 0U)
		&& (s_lock_order_violations == 0U)) {
		fprintf(
			stderr,
			"PM-05A:ALLOC:FAIL ops=%u checks=%u failures=%u "
			"baseline_failures=0 asserts=0 lock_errors=0\n",
			(unsigned int)ITERATIONS,
			s_check_count,
			s_pm_failure_count);
		return 1;
	}
	if ((s_failure_count != 0U)
		|| (s_assert_count != 0U)
		|| (s_lock_order_violations != 0U)) {
		fprintf(
			stderr,
			"CACHEALLOC-02:ALLOC:FAIL ops=%u checks=%u failures=%u "
			"asserts=%u lock_errors=%u\n",
			(unsigned int)ITERATIONS,
			s_check_count,
			s_failure_count,
			s_assert_count,
			s_lock_order_violations);
		return 1;
	}
	printf(
		"CACHEALLOC-02:ALLOC:PASS ops=%u checks=%u "
		"asserts=0 lock_errors=0\n",
		(unsigned int)ITERATIONS,
		s_check_count);
	return 0;
}

int main(int argc, char **argv)
{
	if ((argc == 2) && (strcmp(argv[1], "--assert-count-self-test") == 0)) {
		cachealloc_test_assert(0, "assert-count-self-test");
		return report_result();
	}
	if (argc != 1) {
		fprintf(stderr, "CACHEALLOC-02:ALLOC:HARNESS invalid arguments\n");
		return 2;
	}

	test_allocation_failure_atomicity();
	test_free_failure_atomicity();
	test_forward_merge_retired_line();
	test_guard_owner_span();
	test_owner_span_adjacent_region_boundary();
	test_free_paths_fail_closed();
	test_link_and_sentinel_validation();
	test_free_list_node_validation();
	test_unsplittable_tail_validation();
	test_cycle_rejected_without_hang();
	test_deterministic_two_thread_visibility();
	test_pm_shared_accounting_transitions();

	expect_true("heap lock released", s_lock_depth == 0U);
	expect_true("lock ordering", s_lock_order_violations == 0U);
	return report_result();
}
