#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "armstar.h"
#include "sdkconfig.h"

#ifndef RTOS04_MPU_CASE_NAME
#define RTOS04_MPU_CASE_NAME "unspecified"
#endif

static ARM_MPU_Region_t *s_regions;
static uint32_t s_region_count;
static uint8_t *s_attrs;
static uint32_t s_attr_count;
static unsigned s_checks;
static unsigned s_failures;

void mpu_register_regions(ARM_MPU_Region_t *regions, uint32_t region_count)
{
	s_regions = regions;
	s_region_count = region_count;
}

void mpu_register_attrs(uint8_t *attrs, uint32_t attr_count)
{
	s_attrs = attrs;
	s_attr_count = attr_count;
}

void mpu_soc_cfg(void);

static uint32_t region_base(const ARM_MPU_Region_t *region)
{
	return region->RBAR & 0xffffffe0u;
}

static uint32_t region_limit(const ARM_MPU_Region_t *region)
{
	return (region->RLAR & 0xffffffe0u) | 0x1fu;
}

static uint32_t region_attr(const ARM_MPU_Region_t *region)
{
	return (region->RLAR >> 1) & 0x7u;
}

static uint32_t region_shareability(const ARM_MPU_Region_t *region)
{
	return (region->RBAR >> 3) & 0x3u;
}

static uint32_t region_read_only(const ARM_MPU_Region_t *region)
{
	return (region->RBAR >> 2) & 0x1u;
}

static uint32_t region_non_privileged(const ARM_MPU_Region_t *region)
{
	return (region->RBAR >> 1) & 0x1u;
}

static uint32_t region_execute_never(const ARM_MPU_Region_t *region)
{
	return region->RBAR & 0x1u;
}

static bool region_overlaps(
	const ARM_MPU_Region_t *region,
	uint32_t start,
	uint32_t size)
{
	uint64_t end = (uint64_t)start + size;
	uint64_t region_end = (uint64_t)region_limit(region) + 1u;

	return (uint64_t)region_base(region) < end && region_end > start;
}

static void expect_true(bool condition, const char *message)
{
	s_checks++;
	if (!condition) {
		s_failures++;
		fprintf(stderr, "RTOS-04:MPU:CHECK-FAIL %s\n", message);
	}
}

static const ARM_MPU_Region_t *find_region(uint32_t base, uint32_t limit)
{
	for (uint32_t index = 0; index < s_region_count; index++) {
		if (region_base(&s_regions[index]) == base
		    && region_limit(&s_regions[index]) == limit) {
			return &s_regions[index];
		}
	}
	return NULL;
}

static const char *configuration_name(void)
{
	return RTOS04_MPU_CASE_NAME;
}

int main(void)
{
	const uint32_t stack_end =
		CONFIG_AP_PSRAM_STACK_HEAP_ADDR
		+ CONFIG_AP_PSRAM_STACK_HEAP_SIZE - 1u;
	const ARM_MPU_Region_t *shared_region;
	unsigned psram_wb_regions = 0;
	unsigned psram_regions = 0;

	mpu_soc_cfg();
	expect_true(s_regions != NULL, "MPU regions are registered");
	expect_true(s_region_count > 0u, "MPU region table is nonempty");
	expect_true(s_region_count <= 16u, "MPU region count does not exceed 16");
	expect_true(s_attrs != NULL && s_attr_count > 3u,
		    "MPU Write-Back attribute slot exists");
	if (s_attrs != NULL && s_attr_count > 3u) {
		expect_true(s_attrs[3] == ARM_MPU_ATTR(0xf, 0xf),
			    "attribute index 3 is Write-Back Read/Write Allocate");
	}

	shared_region = find_region(0x28000000u, 0x3fffffffu);
	expect_true(shared_region != NULL, "shared SRAM baseline region is unchanged");
	if (shared_region != NULL) {
		expect_true(region_attr(shared_region) == 1u,
			    "shared SRAM remains non-cacheable");
		expect_true(region_shareability(shared_region) == ARM_MPU_SH_INNER,
			    "shared SRAM shareability remains baseline");
		expect_true(region_read_only(shared_region) == 0u,
			    "shared SRAM remains read/write");
		expect_true(region_non_privileged(shared_region) == 1u,
			    "shared SRAM remains non-privileged accessible");
		expect_true(region_execute_never(shared_region) == 0u,
			    "shared SRAM execute attribute remains baseline");
	}

	for (uint32_t index = 0; index < s_region_count; index++) {
		const ARM_MPU_Region_t *region = &s_regions[index];

		expect_true(region_base(region) <= region_limit(region),
			    "MPU table omits zero-length or inverted regions");
		if (!region_overlaps(
			    region, CONFIG_PSRAM_BASE, CONFIG_PSRAM_CAPACITY)) {
			continue;
		}
		psram_regions++;
		expect_true(region_execute_never(region) == 1u,
			    "all data PSRAM regions are execute-never");
		if (region_attr(region) == 3u) {
			psram_wb_regions++;
			expect_true(region_base(region)
				    == CONFIG_AP_PSRAM_STACK_HEAP_ADDR
				    && region_limit(region) == stack_end,
				    "only the exact stack heap is Write-Back");
		} else {
			expect_true(region_attr(region) == 1u,
				    "ordinary PSRAM remains non-cacheable");
		}
	}

#if CONFIG_CACHEABLE_PSRAM_TASK_STACK && CONFIG_DCACHE
	const ARM_MPU_Region_t *stack_region =
		find_region(CONFIG_AP_PSRAM_STACK_HEAP_ADDR, stack_end);

	expect_true(stack_region != NULL, "feature-on maps the exact stack heap");
	if (stack_region != NULL) {
		expect_true(region_attr(stack_region) == 3u,
			    "stack heap is Write-Back Read/Write Allocate");
		expect_true(region_shareability(stack_region) == ARM_MPU_SH_NON,
			    "stack heap is non-shareable");
		expect_true(region_read_only(stack_region) == 0u,
			    "stack heap is read/write");
		expect_true(region_non_privileged(stack_region) == 1u,
			    "stack heap is non-privileged accessible");
		expect_true(region_execute_never(stack_region) == 1u,
			    "stack heap is execute-never");
	}
	expect_true(psram_wb_regions == 1u,
		    "feature-on has exactly one PSRAM Write-Back region");
	expect_true(
		psram_regions
		    == 1u
		       + (CONFIG_AP_PSRAM_STACK_HEAP_ADDR > CONFIG_PSRAM_BASE)
		       + (stack_end
			  < CONFIG_PSRAM_BASE + CONFIG_PSRAM_CAPACITY - 1u),
		"feature-on emits only nonempty leading/stack/trailing regions");
#else
	expect_true(psram_wb_regions == 0u,
		    "D-cache-off and feature-off PSRAM remain non-cacheable");
	expect_true(psram_regions == 1u,
		    "fallback has exactly one whole-PSRAM region");
	expect_true(find_region(
			    CONFIG_PSRAM_BASE,
			    CONFIG_PSRAM_BASE + CONFIG_PSRAM_CAPACITY - 1u)
		    != NULL,
		    "fallback retains the baseline whole-PSRAM region");
#endif

	if (s_failures != 0u) {
		printf("RTOS-04:MPU:FAIL config=%s checks=%u failures=%u\n",
		       configuration_name(), s_checks, s_failures);
		return 1;
	}

	printf("RTOS-04:MPU:PASS config=%s checks=%u failures=0\n",
	       configuration_name(), s_checks);
	return 0;
}
