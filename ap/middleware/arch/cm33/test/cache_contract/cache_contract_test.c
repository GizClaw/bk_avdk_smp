#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "stubs/armstar.h"
#include "stubs/sdkconfig.h"
#include "cache_psram_stack_internal.h"

typedef int (*cache_maintenance_fn_t)(void *addr, size_t size);

typedef struct {
	const char *name;
	const void *addr;
	size_t size;
	int expected;
} overlap_case_t;

typedef struct {
	const char *name;
	void *addr;
	size_t size;
} invalid_span_t;

typedef struct {
	char kind;
	uintptr_t addr;
	int32_t size;
} cache_operation_t;

#define MAX_OPERATIONS 16U

SCB_Type g_cache_test_scb = {
	.CLIDR = SCB_CLIDR_DC_Msk,
};

static cache_operation_t s_operations[MAX_OPERATIONS];
static size_t s_operation_count;
static unsigned int s_check_count;
static unsigned int s_failure_count;

static void record_operation(char kind, const void *addr, int32_t size)
{
	if (s_operation_count < MAX_OPERATIONS) {
		s_operations[s_operation_count].kind = kind;
		s_operations[s_operation_count].addr = (uintptr_t)addr;
		s_operations[s_operation_count].size = size;
	}
	s_operation_count++;
}

void SCB_CleanDCache_by_Addr(void *addr, int32_t size)
{
	record_operation('C', addr, size);
}

void SCB_InvalidateDCache_by_Addr(void *addr, int32_t size)
{
	record_operation('I', addr, size);
}

void SCB_CleanInvalidateDCache_by_Addr(void *addr, int32_t size)
{
	record_operation('X', addr, size);
}

void SCB_CleanInvalidateDCache(void)
{
	record_operation('A', NULL, 0);
}

void SCB_DisableDCache(void)
{
	record_operation('d', NULL, 0);
}

void SCB_EnableDCache(void)
{
	record_operation('e', NULL, 0);
}

void SCB_InvalidateICache(void)
{
	record_operation('i', NULL, 0);
}

void cache_test_dsb(void)
{
	record_operation('D', NULL, 0);
}

void cache_test_isb(void)
{
	record_operation('B', NULL, 0);
}

static void reset_operations(void)
{
	s_operation_count = 0U;
}

static void expect_int(const char *name, int expected, int actual)
{
	s_check_count++;
	if (expected != actual) {
		fprintf(stderr, "FAIL %s: expected=%d actual=%d\n", name, expected, actual);
		s_failure_count++;
	}
}

static void expect_size(const char *name, size_t expected, size_t actual)
{
	s_check_count++;
	if (expected != actual) {
		fprintf(stderr, "FAIL %s: expected=%zu actual=%zu\n", name, expected, actual);
		s_failure_count++;
	}
}

static void expect_operation(
	const char *name,
	size_t index,
	char expected_kind,
	uintptr_t expected_addr,
	int32_t expected_size)
{
	s_check_count++;
	if (index >= s_operation_count) {
		fprintf(stderr, "FAIL %s: operation %zu missing\n", name, index);
		s_failure_count++;
		return;
	}

	const cache_operation_t *operation = &s_operations[index];
	if ((operation->kind != expected_kind)
		|| (operation->addr != expected_addr)
		|| (operation->size != expected_size)) {
		fprintf(
			stderr,
			"FAIL %s: expected=%c@0x%" PRIxPTR "/%" PRId32
			" actual=%c@0x%" PRIxPTR "/%" PRId32 "\n",
			name,
			expected_kind,
			expected_addr,
			expected_size,
			operation->kind,
			operation->addr,
			operation->size);
		s_failure_count++;
	}
}

static void test_heap_overlap_classifier(void)
{
	const uintptr_t base = (uintptr_t)CONFIG_AP_PSRAM_STACK_HEAP_ADDR;
	const uintptr_t end = base + (uintptr_t)CONFIG_AP_PSRAM_STACK_HEAP_SIZE;
	const size_t signed_negative = (size_t)(-1L);
	const overlap_case_t cases[] = {
		{"null", NULL, 1U, PSRAM_STACK_OVERLAP_INVALID},
		{"zero", (const void *)base, 0U, PSRAM_STACK_OVERLAP_INVALID},
		{"one", (const void *)base, 1U, PSRAM_STACK_OVERLAP},
		{"31", (const void *)(base + 1U), 31U, PSRAM_STACK_OVERLAP},
		{"32", (const void *)(base + 32U), 32U, PSRAM_STACK_OVERLAP},
		{"33", (const void *)(base + 33U), 33U, PSRAM_STACK_OVERLAP},
		{"exact heap", (const void *)base,
			(size_t)CONFIG_AP_PSRAM_STACK_HEAP_SIZE, PSRAM_STACK_OVERLAP},
		{"outside below", (const void *)(base - 32U), 32U,
			PSRAM_STACK_OVERLAP_OUTSIDE},
		{"outside above", (const void *)end, 32U,
			PSRAM_STACK_OVERLAP_OUTSIDE},
		{"left crossing", (const void *)(base - 1U), 2U,
			PSRAM_STACK_OVERLAP},
		{"right crossing", (const void *)(end - 1U), 2U,
			PSRAM_STACK_OVERLAP},
		{"interior unaligned", (const void *)(base + 3U), 1U,
			PSRAM_STACK_OVERLAP},
		{"address overflow", (const void *)(UINTPTR_MAX - 15U), 32U,
			PSRAM_STACK_OVERLAP_INVALID},
		{"SIZE_MAX", (const void *)base, SIZE_MAX,
			PSRAM_STACK_OVERLAP_INVALID},
		{"signed negative adaptation", (const void *)base, signed_negative,
			PSRAM_STACK_OVERLAP_INVALID},
	};

	for (size_t index = 0U; index < (sizeof(cases) / sizeof(cases[0])); index++) {
		expect_int(
			cases[index].name,
			cases[index].expected,
			psram_stack_heap_overlap(cases[index].addr, cases[index].size));
	}
}

static void test_aligned_maintenance_order(void)
{
	const uintptr_t base = (uintptr_t)CONFIG_AP_PSRAM_STACK_HEAP_ADDR;

	reset_operations();
	expect_int(
		"release exact heap",
		0,
		cache_stack_release_aligned(
			(void *)base,
			(size_t)CONFIG_AP_PSRAM_STACK_HEAP_SIZE));
	expect_size("release operation count", 4U, s_operation_count);
	expect_operation(
		"release clean",
		0U,
		'C',
		base,
		(int32_t)CONFIG_AP_PSRAM_STACK_HEAP_SIZE);
	expect_operation("release first barrier", 1U, 'D', 0U, 0);
	expect_operation(
		"release invalidate",
		2U,
		'I',
		base,
		(int32_t)CONFIG_AP_PSRAM_STACK_HEAP_SIZE);
	expect_operation("release second barrier", 3U, 'D', 0U, 0);

	reset_operations();
	expect_int(
		"acquire aligned",
		0,
		cache_stack_acquire_aligned((void *)(base + 32U), 32U));
	expect_size("acquire operation count", 2U, s_operation_count);
	expect_operation("acquire invalidate", 0U, 'I', base + 32U, 32);
	expect_operation("acquire barrier", 1U, 'D', 0U, 0);

	reset_operations();
	expect_int(
		"metadata aligned",
		0,
		cache_stack_metadata_clean_aligned((void *)(base + 64U), 32U));
	expect_size("metadata operation count", 2U, s_operation_count);
	expect_operation("metadata clean", 0U, 'C', base + 64U, 32);
	expect_operation("metadata barrier", 1U, 'D', 0U, 0);
}

static void expect_invalid_noop(
	const char *helper_name,
	cache_maintenance_fn_t helper,
	const invalid_span_t *span)
{
	reset_operations();
	s_check_count++;
	if (helper(span->addr, span->size) == 0) {
		fprintf(stderr, "FAIL %s %s: expected rejection\n", helper_name, span->name);
		s_failure_count++;
	}
	expect_size(span->name, 0U, s_operation_count);
}

static void test_helper_by_invalid_zero_operations(void)
{
	const uintptr_t base = (uintptr_t)CONFIG_AP_PSRAM_STACK_HEAP_ADDR;
	const uintptr_t end = base + (uintptr_t)CONFIG_AP_PSRAM_STACK_HEAP_SIZE;
	const size_t signed_negative = (size_t)(-1L);
	const invalid_span_t spans[] = {
		{"null", NULL, 32U},
		{"zero", (void *)base, 0U},
		{"unaligned start", (void *)(base + 1U), 32U},
		{"size 1", (void *)base, 1U},
		{"size 31", (void *)base, 31U},
		{"size 33", (void *)base, 33U},
		{"outside below", (void *)(base - 32U), 32U},
		{"outside above", (void *)end, 32U},
		{"left crossing", (void *)(base - 32U), 64U},
		{"right crossing", (void *)(end - 32U), 64U},
		{"address overflow", (void *)(UINTPTR_MAX - 31U), 64U},
		{"SIZE_MAX", (void *)base, SIZE_MAX},
		{"signed negative adaptation", (void *)base, signed_negative},
		{"CMSIS signed overflow", (void *)base, (size_t)INT32_MAX + 1U},
	};
	const struct {
		const char *name;
		cache_maintenance_fn_t function;
	} helpers[] = {
		{"release", cache_stack_release_aligned},
		{"acquire", cache_stack_acquire_aligned},
		{"metadata", cache_stack_metadata_clean_aligned},
	};

	for (size_t helper_index = 0U;
		helper_index < (sizeof(helpers) / sizeof(helpers[0]));
		helper_index++) {
		for (size_t span_index = 0U;
			span_index < (sizeof(spans) / sizeof(spans[0]));
			span_index++) {
			expect_invalid_noop(
				helpers[helper_index].name,
				helpers[helper_index].function,
				&spans[span_index]);
		}
	}
}

int main(void)
{
	test_heap_overlap_classifier();
	test_aligned_maintenance_order();
	test_helper_by_invalid_zero_operations();

	if (s_failure_count != 0U) {
		fprintf(
			stderr,
			"CACHEALLOC-02:CACHE:FAIL checks=%u failures=%u\n",
			s_check_count,
			s_failure_count);
		return 1;
	}

	printf("CACHEALLOC-02:CACHE:PASS checks=%u\n", s_check_count);
	return 0;
}
