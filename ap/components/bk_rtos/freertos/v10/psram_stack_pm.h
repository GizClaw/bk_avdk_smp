#ifndef PSRAM_STACK_PM_H
#define PSRAM_STACK_PM_H

#include "sdkconfig.h"
#include <stdint.h>
#include <common/bk_err.h>

#if defined(CONFIG_CACHEABLE_PSRAM_TASK_STACK) && CONFIG_CACHEABLE_PSRAM_TASK_STACK

bk_err_t bk_rtos_psram_stack_pm_ensure_ready(void);
bk_err_t bk_rtos_psram_stack_pm_vote_active(void);
bk_err_t bk_rtos_psram_stack_pm_vote_released(void);
bk_err_t bk_rtos_psram_stack_pm_resync_pending(
	uint64_t pending_generation);

#endif

#endif
