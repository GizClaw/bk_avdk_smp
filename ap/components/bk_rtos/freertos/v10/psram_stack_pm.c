#include "sdkconfig.h"

#if defined(CONFIG_CACHEABLE_PSRAM_TASK_STACK) && CONFIG_CACHEABLE_PSRAM_TASK_STACK

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <modules/pm.h>
#include <os/os.h>
#include "psram_stack_pm.h"

typedef enum {
	PSRAM_STACK_PM_UNSYNCED = 0,
	PSRAM_STACK_PM_RESYNC_INFLIGHT,
	PSRAM_STACK_PM_READY,
	PSRAM_STACK_PM_LOCKOUT,
} psram_stack_pm_readiness_t;

static volatile uint32_t s_psram_stack_pm_readiness
	__attribute__((section(".dtcm_sec_data"))) = PSRAM_STACK_PM_UNSYNCED;

_Static_assert(sizeof(uint64_t) == 8U,
	"PSRAM stack PM generation tokens require 64 bits");

int psram_stack_pm_resync_claim(uint64_t *generation);
int psram_stack_pm_resync_commit(uint64_t generation);
int psram_stack_pm_resync_abort(uint64_t generation);

bk_err_t bk_pm_psram_stack_vote_sleep_ctrl(uint32_t sleep_state)
	__attribute__((weak));

/* The provider owns lockout, resync, and inflight transaction state. */

static bk_err_t bk_rtos_psram_stack_pm_vote(uint32_t state)
{
	bk_err_t (*vote_fn)(uint32_t) =
		bk_pm_psram_stack_vote_sleep_ctrl;

	if ((state > 1U)
		|| rtos_is_in_interrupt_context()
		|| rtos_local_irq_disabled()) {
		return BK_FAIL;
	}
	if (vote_fn != NULL) {
		return vote_fn(state);
	}
	return bk_pm_module_vote_sleep_ctrl(
		PM_SLEEP_MODULE_NAME_PSRAM, state, 0);
}

bk_err_t bk_rtos_psram_stack_pm_ensure_ready(void)
{
	uint32_t expected = PSRAM_STACK_PM_UNSYNCED;
	bk_err_t result;
	bk_err_t configPSRAM_STACK_PM_VOTE_RELEASED_result;
	bool resync_ready;

	if (__atomic_load_n(
			&s_psram_stack_pm_readiness, __ATOMIC_ACQUIRE)
		== PSRAM_STACK_PM_READY) {
		return BK_OK;
	}
	if (!__atomic_compare_exchange_n(
			&s_psram_stack_pm_readiness, &expected,
			PSRAM_STACK_PM_RESYNC_INFLIGHT, false,
			__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		return BK_FAIL;
	}
	result = bk_rtos_psram_stack_pm_vote(1U);
	configPSRAM_STACK_PM_VOTE_RELEASED_result = result;
	if (configPSRAM_STACK_PM_VOTE_RELEASED_result != BK_OK) {
		__atomic_store_n(
			&s_psram_stack_pm_readiness,
			PSRAM_STACK_PM_UNSYNCED, __ATOMIC_RELEASE);
		return result;
	}
	resync_ready = true;
	__atomic_store_n(
		&s_psram_stack_pm_readiness,
		resync_ready ? PSRAM_STACK_PM_READY : PSRAM_STACK_PM_UNSYNCED,
		__ATOMIC_RELEASE);
	return result;
}

bk_err_t bk_rtos_psram_stack_pm_vote_active(void)
{
	if (__atomic_load_n(
			&s_psram_stack_pm_readiness, __ATOMIC_ACQUIRE)
		!= PSRAM_STACK_PM_READY) {
		return BK_FAIL;
	}
	return bk_rtos_psram_stack_pm_vote(0U);
}

bk_err_t bk_rtos_psram_stack_pm_vote_released(void)
{
	const bk_err_t result = bk_rtos_psram_stack_pm_vote(1U);

	if (result == BK_OK) {
		__atomic_store_n(
			&s_psram_stack_pm_readiness,
			PSRAM_STACK_PM_READY, __ATOMIC_RELEASE);
	}
	return result;
}

bk_err_t bk_rtos_psram_stack_pm_resync_pending(
	uint64_t pending_generation)
{
	if (pending_generation == 0U) {
		const bool locked_out = true;

		__atomic_store_n(
			&s_psram_stack_pm_readiness,
			locked_out ? PSRAM_STACK_PM_LOCKOUT
				: PSRAM_STACK_PM_UNSYNCED,
			__ATOMIC_RELEASE);
		return BK_FAIL;
	}
	return bk_rtos_psram_stack_pm_vote_released();
}

#endif
