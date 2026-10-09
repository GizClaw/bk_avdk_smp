#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <common/bk_err.h>
#include <os/os.h>

static unsigned s_dynamic_init_calls;
static unsigned s_dynamic_take_calls;
static unsigned s_dynamic_give_calls;
static unsigned s_static_init_calls;
static unsigned s_static_take_calls;
static unsigned s_static_give_calls;
static unsigned s_vote_calls;
static bk_err_t s_vote_result = BK_OK;
static unsigned s_checks;
static unsigned s_failures;
static int s_mutex_token;

bk_err_t rtos_init_mutex(beken_mutex_t *mutex)
{
	s_dynamic_init_calls++;
	*mutex = &s_mutex_token;
	return BK_OK;
}

bk_err_t rtos_lock_mutex(beken_mutex_t *mutex)
{
	s_dynamic_take_calls++;
	return *mutex == NULL ? BK_FAIL : BK_OK;
}

bk_err_t rtos_unlock_mutex(beken_mutex_t *mutex)
{
	s_dynamic_give_calls++;
	return *mutex == NULL ? BK_FAIL : BK_OK;
}

bool rtos_is_in_interrupt_context(void)
{
	return false;
}

bool rtos_local_irq_disabled(void)
{
	return false;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
	s_static_init_calls++;
	return storage;
}

int xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t timeout)
{
	(void)timeout;
	s_static_take_calls++;
	return mutex == NULL ? 0 : pdPASS;
}

int xSemaphoreGive(SemaphoreHandle_t mutex)
{
	s_static_give_calls++;
	return mutex == NULL ? 0 : pdPASS;
}

bk_err_t bk_pm_module_vote_sleep_ctrl(
	uint32_t module,
	uint32_t sleep_state,
	uint32_t sleep_time)
{
	(void)module;
	(void)sleep_state;
	(void)sleep_time;
	s_vote_calls++;
	return s_vote_result;
}

#include PM05C_FACADE_SOURCE

static void expect_true(bool condition, const char *name)
{
	s_checks++;
	if (!condition) {
		s_failures++;
		fprintf(stderr, "PM-05C:FACADE-ACTUAL:CHECK-FAIL %s\n", name);
	}
}

int main(void)
{
	expect_true(
		bk_rtos_psram_stack_pm_ensure_ready() == BK_OK
			&& bk_rtos_psram_stack_pm_vote_active() == BK_OK
			&& s_vote_calls == 2U,
		"production facade executes shared resync and active vote");
	expect_true(
		s_static_init_calls == 0U
			&& s_static_take_calls == 0U
			&& s_static_give_calls == 0U
			&& s_dynamic_init_calls == 0U,
		"production facade uses no task-owned mutex");
	s_vote_result = BK_FAIL;
	expect_true(
		bk_rtos_psram_stack_pm_vote_released() == BK_FAIL,
		"production facade propagates release NACK");
	expect_true(
		s_static_take_calls == 0U
			&& s_static_give_calls == 0U
			&& s_dynamic_take_calls == 0U
			&& s_dynamic_give_calls == 0U,
		"release failure converges shared state without participant");

	printf(
		"PM-05C:FACADE-ACTUAL:%s checks=%u failures=%u\n",
		s_failures == 0U ? "PASS" : "FAIL",
		s_checks,
		s_failures);
	return s_failures == 0U ? 0 : 1;
}
