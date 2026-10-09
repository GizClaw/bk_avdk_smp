#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../../heap_4.c"

_Alignas(32) uint8_t g_psram_stack_test_heap[4096];
_Alignas(32) uint8_t g_psram_test_ordinary_heap[4096];
uint32_t g_fixed_psram_power_down;
uint32_t g_fixed_psram_used_count;

static bool s_heap_initialized;
static unsigned int s_lock_depth;
static unsigned int s_power_on_calls;
static unsigned int s_power_on_lock_depth;
static int s_power_on_result = BK_OK;

void cachealloc_test_assert(int condition, const char *expression)
{
	if (condition == 0) {
		fprintf(stderr, "PM-05A:FEATURE-OFF:ASSERT %s\n", expression);
	}
}

void pm05a_test_fatal_reset(void)
{
}

void pm05a_test_dmb(void)
{
	__asm__ volatile ("" ::: "memory");
}

void vPortEnterCritical(spinlock_t *lock)
{
	(void)pthread_mutex_lock(&lock->mutex);
	s_lock_depth++;
}

void vPortExitCritical(spinlock_t *lock)
{
	s_lock_depth--;
	(void)pthread_mutex_unlock(&lock->mutex);
}

bool bk_psram_heap_init_flag_get(void)
{
	return s_heap_initialized;
}

void bk_psram_heap_init_flag_set(bool initialized)
{
	s_heap_initialized = initialized;
}

int bk_pm_module_vote_psram_ctrl(int module, int state)
{
	(void)module;
	(void)state;
	s_power_on_calls++;
	s_power_on_lock_depth = s_lock_depth;
	return s_power_on_result;
}

UBaseType_t xTaskGetTickCount(void)
{
	return 0U;
}

const char *pcTaskGetName(TaskHandle_t task)
{
	(void)task;
	return "pm05a-feature-off";
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

int main(void)
{
	FIXED_ADDR_PSRAM_USDE_COUNT = 0U;
	s_psram_used_count = 0U;
	s_heap_initialized = false;
	psram_pxEnd = NULL;
	xBlockAllocatedBit =
		((size_t)1U) << ((sizeof(size_t) * 8U) - 1U);

	s_power_on_result = BK_FAIL;
	void *payload = psram_malloc_cm(__func__, __LINE__, 32U, 0);
	if ((payload != NULL) || (psram_pxEnd != NULL)
		|| (s_psram_used_count != 0U)
		|| (FIXED_ADDR_PSRAM_USDE_COUNT != 0U)
		|| (s_psram_power_inflight != 0U)
		|| (s_psram_stack_pm_provisional_hold != 0U)) {
		printf("PM-05A:FEATURE-OFF:HARNESS-FAIL power-failure\n");
		return 2;
	}
	s_power_on_result = BK_OK;
	payload = psram_malloc_cm(__func__, __LINE__, 32U, 0);
	if ((payload == NULL) || (psram_pxEnd == NULL)) {
		printf("PM-05A:FEATURE-OFF:HARNESS-FAIL initialize\n");
		return 2;
	}
	vPortFree(payload);
	if ((s_psram_used_count != 0U)
		|| (FIXED_ADDR_PSRAM_USDE_COUNT != 0U)) {
		printf("PM-05A:FEATURE-OFF:HARNESS-FAIL cleanup\n");
		return 2;
	}

	psram_xStart.pxNextFreeBlock = psram_pxEnd;
	psram_pxEnd->xBlockSize = portBYTE_ALIGNMENT;
	FIXED_ADDR_PSRAM_POWER_DOWN = PM_PSRAM_POWER_DOWN_MAGIC;
	s_power_on_calls = 0U;
	s_power_on_lock_depth = 0U;
	payload = psram_malloc_cm(__func__, __LINE__, 32U, 0);
	if ((payload == NULL)
		|| !s_heap_initialized
		|| (s_power_on_calls != 1U)
		|| (s_power_on_lock_depth != 0U)
		|| (FIXED_ADDR_PSRAM_POWER_DOWN != 0U)
		|| (psram_pxEnd == NULL)
		|| (psram_pxEnd->xBlockSize != 0U)
		|| (s_psram_used_count != 1U)
		|| (FIXED_ADDR_PSRAM_USDE_COUNT != 1U)) {
		printf(
			"PM-05A:FEATURE-OFF:FAIL payload=%u init=%u power=%u "
			"power_lock=%u magic=%u shared=%u fixed=%u\n",
			payload != NULL,
			s_heap_initialized,
			s_power_on_calls,
			s_power_on_lock_depth,
			(unsigned int)FIXED_ADDR_PSRAM_POWER_DOWN,
			(unsigned int)s_psram_used_count,
			(unsigned int)FIXED_ADDR_PSRAM_USDE_COUNT);
		return 1;
	}
	printf(
		"PM-05A:FEATURE-OFF:PASS initialized_magic_rebuild=1 "
		"power_outside_lock=1 shared=1 fixed=1\n");
	return 0;
}
