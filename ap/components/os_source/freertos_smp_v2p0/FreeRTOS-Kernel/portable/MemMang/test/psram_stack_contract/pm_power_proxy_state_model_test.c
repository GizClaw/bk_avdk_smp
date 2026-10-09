#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
	PROXY_IDLE = 0,
	PROXY_PREPARING,
	PROXY_WAIT_ACK,
	PROXY_DONE,
	PROXY_QUARANTINE,
} proxy_state_t;

typedef struct {
	proxy_state_t state;
	uint32_t next_cycle;
	uint32_t active_cycle;
	int result;
	uint32_t result_cycle;
	uint32_t callbacks;
	uint32_t sends;
	uint32_t discarded_acks;
} proxy_model_t;

static unsigned s_checks;
static unsigned s_failures;

static void expect_true(bool condition, const char *name)
{
	s_checks++;
	if (!condition) {
		s_failures++;
		fprintf(stderr, "PM-05A:PROXY-MODEL:CHECK-FAIL %s\n", name);
	}
}

static int proxy_claim(proxy_model_t *model, uint32_t *cycle)
{
	if (model->state != PROXY_IDLE) {
		return -1;
	}
	model->next_cycle++;
	if (model->next_cycle == 0U) {
		model->next_cycle++;
	}
	model->active_cycle = model->next_cycle;
	model->state = PROXY_PREPARING;
	*cycle = model->active_cycle;
	return 0;
}

static int proxy_publish(proxy_model_t *model, uint32_t cycle)
{
	if ((model->state != PROXY_PREPARING)
		|| (model->active_cycle != cycle)) {
		return -1;
	}
	model->result = -1;
	model->result_cycle = 0U;
	model->state = PROXY_WAIT_ACK;
	return 0;
}

static int proxy_send(proxy_model_t *model, uint32_t cycle)
{
	if ((model->state != PROXY_WAIT_ACK)
		|| (model->active_cycle != cycle)) {
		return -1;
	}
	model->sends++;
	return 0;
}

static int proxy_timeout(proxy_model_t *model, uint32_t cycle)
{
	if ((model->state != PROXY_WAIT_ACK)
		|| (model->active_cycle != cycle)) {
		return -1;
	}
	model->state = PROXY_QUARANTINE;
	return -1;
}

static int proxy_ack(
	proxy_model_t *model,
	uint32_t cycle,
	int result)
{
	if (model->active_cycle != cycle) {
		return -1;
	}
	if (model->state == PROXY_PREPARING) {
		model->discarded_acks++;
		return -1;
	}
	if (model->state == PROXY_QUARANTINE) {
		model->state = PROXY_IDLE;
		model->active_cycle = 0U;
		return -1;
	}
	if (model->state != PROXY_WAIT_ACK) {
		return -1;
	}
	model->result = result;
	model->result_cycle = cycle;
	model->state = PROXY_DONE;
	return 0;
}

static int proxy_complete(
	proxy_model_t *model,
	uint32_t cycle,
	bool start_next,
	uint32_t *next_cycle)
{
	if ((model->state != PROXY_DONE)
		|| (model->active_cycle != cycle)) {
		return -1;
	}
	const int local_result = model->result;
	model->state = PROXY_IDLE;
	model->active_cycle = 0U;
	if (start_next && (proxy_claim(model, next_cycle) == 0)) {
		(void)proxy_publish(model, *next_cycle);
	}
	return local_result;
}

int main(void)
{
	proxy_model_t model = {0};
	uint32_t first_cycle;
	uint32_t second_cycle;
	uint32_t third_cycle;

	expect_true(
		proxy_claim(&model, &first_cycle) == 0
			&& model.state == PROXY_PREPARING,
		"first caller owns PREPARING before publication");
	expect_true(
		proxy_ack(&model, first_cycle, 0) != 0
			&& model.state == PROXY_PREPARING
			&& model.discarded_acks == 1U
			&& model.result_cycle == 0U,
		"ISR discards an ACK observed during PREPARING");
	expect_true(
		proxy_publish(&model, first_cycle) == 0
			&& proxy_send(&model, first_cycle) == 0
			&& model.state == PROXY_WAIT_ACK
			&& model.sends == 1U,
		"request and result initialize before WAIT release and send");
	expect_true(
		proxy_claim(&model, &second_cycle) != 0,
		"concurrent caller is rejected");
	expect_true(
		proxy_timeout(&model, first_cycle) != 0
			&& model.state == PROXY_QUARANTINE,
		"timeout enters quarantine");
	expect_true(
		proxy_claim(&model, &second_cycle) != 0,
		"new request is rejected before late ACK");
	expect_true(
		proxy_ack(&model, first_cycle, 0) != 0
			&& model.state == PROXY_IDLE
			&& model.callbacks == 0U
			&& model.result_cycle == 0U,
		"late ACK only clears the old quarantined cycle");
	expect_true(
		proxy_claim(&model, &second_cycle) == 0
			&& proxy_publish(&model, second_cycle) == 0
			&& second_cycle != first_cycle,
		"new cycle starts only after quarantine clears");
	expect_true(
		proxy_ack(&model, first_cycle, 0) != 0
			&& model.state == PROXY_WAIT_ACK
			&& model.callbacks == 0U,
		"stale ACK cannot publish into a new cycle");
	expect_true(
		proxy_ack(&model, second_cycle, 7) == 0
			&& model.state == PROXY_DONE
			&& model.result_cycle == second_cycle,
		"matching ISR publishes DONE without releasing ownership");
	const int first_local_result =
		proxy_complete(&model, second_cycle, true, &third_cycle);
	expect_true(
		first_local_result == 7
			&& model.state == PROXY_WAIT_ACK
			&& model.active_cycle == third_cycle
			&& model.result == -1,
		"complete captures local result before IDLE lets next request overwrite global");
	model.callbacks += first_local_result == 0;
	expect_true(
		model.callbacks == 0U && first_local_result == 7,
		"caller outcome uses only the captured local result");

	if (s_failures != 0U) {
		printf(
			"PM-05A:PROXY-MODEL:FAIL checks=%u failures=%u\n",
			s_checks,
			s_failures);
		return 1;
	}
	printf(
		"PM-05A:PROXY-MODEL:PASS checks=%u failures=0\n",
		s_checks);
	return 0;
}
