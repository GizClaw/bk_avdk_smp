#ifndef PM05C_STUB_MODULES_PM_H
#define PM05C_STUB_MODULES_PM_H

#include <stdint.h>
#include <common/bk_err.h>

#define PM_SLEEP_MODULE_NAME_PSRAM 7U

bk_err_t bk_pm_module_vote_sleep_ctrl(
	uint32_t module,
	uint32_t sleep_state,
	uint32_t sleep_time);

#endif
