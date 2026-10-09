// Copyright 2023-2028 Beken
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stddef.h>
#include <stdint.h>
#include "armstar.h"
#include "sdkconfig.h"
#include "cache.h"
#include "cache_psram_stack_internal.h"
#include "mpu.h"

void sram_dcache_map(void)
{
}

int show_cache_config_info(void)
{
	return 0;
}

#if defined(CONFIG_CACHEABLE_PSRAM_TASK_STACK) && CONFIG_CACHEABLE_PSRAM_TASK_STACK

#define BK_PSRAM_STACK_CACHE_LINE_SIZE 32U
#define BK_CACHE_LINE_SIZE ((uintptr_t)__SCB_DCACHE_LINE_SIZE)
#define BK_CACHE_LINE_MASK (BK_CACHE_LINE_SIZE - 1U)

__attribute__((section(".iram"))) static int cache_line_size_valid(void)
{
	return (BK_CACHE_LINE_SIZE == BK_PSRAM_STACK_CACHE_LINE_SIZE)
		&& ((BK_CACHE_LINE_SIZE & BK_CACHE_LINE_MASK) == 0U);
}

__attribute__((section(".iram"))) static int cache_checked_range(
	const void *addr,
	size_t size,
	uintptr_t *start,
	uintptr_t *end)
{
	const uintptr_t range_start = (uintptr_t)addr;

	if ((addr == NULL) || (size == 0U)) {
		return -1;
	}
	if (size > (size_t)(UINTPTR_MAX - range_start)) {
		return -1;
	}

	*start = range_start;
	*end = range_start + (uintptr_t)size;
	return 0;
}

__attribute__((section(".iram"))) static int psram_stack_heap_bounds(
	uintptr_t *heap_start,
	uintptr_t *heap_end)
{
	const uintptr_t start = (uintptr_t)CONFIG_AP_PSRAM_STACK_HEAP_ADDR;
	const size_t size = (size_t)CONFIG_AP_PSRAM_STACK_HEAP_SIZE;

	if (!cache_line_size_valid()
		|| (size == 0U)
		|| ((start & BK_CACHE_LINE_MASK) != 0U)
		|| ((size & (size_t)BK_CACHE_LINE_MASK) != 0U)
		|| (size > (size_t)(UINTPTR_MAX - start))) {
		return -1;
	}

	*heap_start = start;
	*heap_end = start + (uintptr_t)size;
	return 0;
}

__attribute__((section(".iram"))) psram_stack_overlap_t
psram_stack_heap_overlap(const void *addr, size_t size)
{
	uintptr_t heap_start;
	uintptr_t heap_end;
	uintptr_t start;
	uintptr_t end;

	if ((cache_checked_range(addr, size, &start, &end) != 0)
		|| (psram_stack_heap_bounds(&heap_start, &heap_end) != 0)) {
		return PSRAM_STACK_OVERLAP_INVALID;
	}

	if ((end <= heap_start) || (start >= heap_end)) {
		return PSRAM_STACK_OVERLAP_OUTSIDE;
	}
	return PSRAM_STACK_OVERLAP;
}

__attribute__((section(".iram"))) static int cache_stack_prepare_span(
	const void *addr,
	size_t size,
	int32_t *maintenance_size)
{
	uintptr_t heap_start;
	uintptr_t heap_end;
	uintptr_t start;
	uintptr_t end;

	if ((cache_checked_range(addr, size, &start, &end) != 0)
		|| (psram_stack_heap_bounds(&heap_start, &heap_end) != 0)
		|| ((start & BK_CACHE_LINE_MASK) != 0U)
		|| ((size & (size_t)BK_CACHE_LINE_MASK) != 0U)
		|| (size > (size_t)INT32_MAX)
		|| (start < heap_start)
		|| (end > heap_end)) {
		return -1;
	}

	*maintenance_size = (int32_t)size;
	return 0;
}

__attribute__((section(".iram"))) int cache_stack_release_aligned(
	void *addr, size_t size)
{
	int32_t maintenance_size;

	if ((cache_stack_prepare_span(addr, size, &maintenance_size) != 0)
		|| ((SCB->CLIDR & SCB_CLIDR_DC_Msk) == 0U)) {
		return -1;
	}

	SCB_CleanDCache_by_Addr(addr, maintenance_size);
	__DSB();
	SCB_InvalidateDCache_by_Addr(addr, maintenance_size);
	__DSB();
	return 0;
}

__attribute__((section(".iram"))) int cache_stack_acquire_aligned(
	void *addr, size_t size)
{
	int32_t maintenance_size;

	if ((cache_stack_prepare_span(addr, size, &maintenance_size) != 0)
		|| ((SCB->CLIDR & SCB_CLIDR_DC_Msk) == 0U)) {
		return -1;
	}

	SCB_InvalidateDCache_by_Addr(addr, maintenance_size);
	__DSB();
	return 0;
}

__attribute__((section(".iram"))) int cache_stack_metadata_clean_aligned(
	void *addr, size_t size)
{
	int32_t maintenance_size;

	if ((cache_stack_prepare_span(addr, size, &maintenance_size) != 0)
		|| ((SCB->CLIDR & SCB_CLIDR_DC_Msk) == 0U)) {
		return -1;
	}

	SCB_CleanDCache_by_Addr(addr, maintenance_size);
	__DSB();
	return 0;
}

__attribute__((section(".iram"))) void cache_stack_metadata_clean_commit(
	void *addr, size_t size)
{
	/* The transactional allocator prevalidates this exact aligned span. */
	SCB_CleanDCache_by_Addr(addr, (int32_t)size);
	__DSB();
}

__attribute__((section(".iram"))) void cache_stack_metadata_retire_commit(
	void *addr, size_t size)
{
	/* Retired metadata must not remain dirty after its line becomes payload. */
	SCB_InvalidateDCache_by_Addr(addr, (int32_t)size);
	__DSB();
}

#endif

__attribute__((section(".iram"))) void flush_dcache(void *va, long size)
{
    if (SCB->CLIDR & SCB_CLIDR_DC_Msk) {
        SCB_CleanInvalidateDCache_by_Addr(va, size);
    }
}

__attribute__((section(".iram"))) void invalidate_dcache(void *va, long size)
{
    if (SCB->CLIDR & SCB_CLIDR_DC_Msk) {
        SCB_InvalidateDCache_by_Addr(va, size);
    }
}

void flush_all_dcache(void)
{
    if (SCB->CLIDR & SCB_CLIDR_DC_Msk) {
        SCB_CleanInvalidateDCache();
    }
}

void enable_dcache(int enable)
{
    if (enable == 0) {
        SCB_DisableDCache();
    } else {
        SCB_EnableDCache();
    }
}

void invalidate_icache(void)
{
    SCB_InvalidateICache();
}
// eof

