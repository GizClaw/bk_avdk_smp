#ifndef PSRAM_STACK_TEST_PWR_CLK_H
#define PSRAM_STACK_TEST_PWR_CLK_H

#define BK_OK 0
#define BK_FAIL (-1)

int bk_pm_module_vote_psram_ctrl(int module, int state);

#endif
