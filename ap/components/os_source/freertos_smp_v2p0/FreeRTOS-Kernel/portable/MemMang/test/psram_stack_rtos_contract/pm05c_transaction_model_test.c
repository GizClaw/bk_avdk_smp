#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
	PM_PSRAM_STACK_IDLE = 0,
	PM_PSRAM_STACK_PREPARING,
	PM_PSRAM_STACK_WAITING,
	PM_PSRAM_STACK_TIMED_OUT,
	PM_PSRAM_STACK_DONE,
} transaction_state_t;

typedef struct {
	uint32_t low;
	uint32_t high;
} shared_u64_words_t;

typedef struct {
	transaction_state_t state;
	uint32_t request_generation;
	uint32_t active_request;
	int result;
	uint64_t allocator_hold_generation;
	bool locked_out;
	uint32_t fatal_calls;
	uint32_t quarantined_acks;
	uint32_t generic_discards;
} transaction_model_t;

static unsigned s_checks;
static unsigned s_failures;

static uint64_t shared_u64_load(const shared_u64_words_t *field)
{
	const uint32_t high = field->high;
	const uint32_t low = field->low;

	return ((uint64_t)high << 32) | low;
}

static void shared_u64_store(shared_u64_words_t *field, uint64_t value)
{
	field->low = (uint32_t)value;
	field->high = (uint32_t)(value >> 32);
}

static void expect_true(bool condition, const char *name)
{
	s_checks++;
	if (!condition) {
		s_failures++;
		fprintf(stderr, "PM-05C:MODEL:CHECK-FAIL %s\n", name);
	}
}

static int begin(transaction_model_t *model, uint32_t *request)
{
	if (model->locked_out || model->state != PM_PSRAM_STACK_IDLE) {
		return -1;
	}
	model->state = PM_PSRAM_STACK_PREPARING;
	if (model->request_generation == UINT32_MAX) {
		model->locked_out = true;
		model->fatal_calls++;
		model->state = PM_PSRAM_STACK_IDLE;
		return -1;
	}
	model->request_generation++;
	model->active_request = model->request_generation;
	model->result = -1;
	*request = model->active_request;
	model->state = PM_PSRAM_STACK_WAITING;
	return 0;
}

static int generic_ack(transaction_model_t *model)
{
	model->generic_discards++;
	return -1;
}

static int dedicated_ack(
	transaction_model_t *model,
	uint32_t request,
	int result)
{
	if (request != model->active_request) {
		return -1;
	}
	if (model->state == PM_PSRAM_STACK_TIMED_OUT) {
		model->quarantined_acks++;
		model->active_request = 0U;
		model->state = PM_PSRAM_STACK_IDLE;
		return -1;
	}
	if (model->state != PM_PSRAM_STACK_WAITING) {
		return -1;
	}
	model->result = result;
	model->state = PM_PSRAM_STACK_DONE;
	return 0;
}

static int timeout(transaction_model_t *model, uint32_t request)
{
	if ((model->state != PM_PSRAM_STACK_WAITING)
		|| (model->active_request != request)) {
		return -1;
	}
	model->state = PM_PSRAM_STACK_TIMED_OUT;
	return -1;
}

static int complete(transaction_model_t *model, uint32_t request)
{
	if ((model->state != PM_PSRAM_STACK_DONE)
		|| (model->active_request != request)) {
		return -1;
	}
	const int local_result = model->result;
	model->active_request = 0U;
	model->state = PM_PSRAM_STACK_IDLE;
	return local_result;
}

int main(void)
{
	transaction_model_t model = {0};
	uint32_t first_request;
	uint32_t second_request;
	shared_u64_words_t shared = {0};

	shared_u64_store(&shared, UINT64_C(0x0123456789abcdef));
	expect_true(
		sizeof(shared_u64_words_t) == 8U
			&& shared.low == UINT32_C(0x89abcdef)
			&& shared.high == UINT32_C(0x01234567)
			&& shared_u64_load(&shared)
				== UINT64_C(0x0123456789abcdef),
		"shared fixed64 uses ordered low and high 32-bit words");
	expect_true(
		begin(&model, &first_request) == 0
			&& model.state == PM_PSRAM_STACK_WAITING,
		"claim prepares and publishes waiting");
	expect_true(
		begin(&model, &second_request) != 0,
		"concurrent transaction fails");
	expect_true(
		generic_ack(&model) != 0
			&& model.state == PM_PSRAM_STACK_WAITING,
		"generic ACK cannot complete dedicated transaction");
	expect_true(
		timeout(&model, first_request) != 0
			&& model.state == PM_PSRAM_STACK_TIMED_OUT,
		"timeout enters timed-out quarantine");
	expect_true(
		begin(&model, &second_request) != 0,
		"new transaction fails while timed-out request is quarantined");
	expect_true(
		dedicated_ack(&model, first_request + 1U, 0) != 0
			&& model.state == PM_PSRAM_STACK_TIMED_OUT,
		"nonmatching late ACK cannot clear quarantine");
	expect_true(
		dedicated_ack(&model, first_request, 0) != 0
			&& model.quarantined_acks == 1U
			&& model.state == PM_PSRAM_STACK_IDLE,
		"late matching ACK clears only its quarantined cycle");
	(void)begin(&model, &second_request);
	expect_true(
		dedicated_ack(&model, second_request, -7) == 0
			&& complete(&model, second_request) == -7,
		"NACK result returns failure");
	(void)begin(&model, &second_request);
	expect_true(
		dedicated_ack(&model, second_request, 0) == 0
			&& complete(&model, second_request) == 0,
		"matching success ACK completes exact request");
	model.state = PM_PSRAM_STACK_IDLE;
	model.request_generation = UINT32_MAX - 1U;
	expect_true(
		begin(&model, &second_request) == 0
			&& second_request == UINT32_MAX,
		"UINT32_MAX wire request generation is emitted once");
	model.state = PM_PSRAM_STACK_IDLE;
	model.allocator_hold_generation = UINT64_MAX;
	expect_true(
		begin(&model, &second_request) != 0
			&& model.locked_out
			&& model.fatal_calls == 1U
			&& model.allocator_hold_generation == UINT64_MAX,
		"wire generation exhaustion locks out without narrowing allocator hold");

	printf(
		"PM-05C:MODEL:%s checks=%u failures=%u\n",
		s_failures == 0U ? "PASS" : "FAIL",
		s_checks,
		s_failures);
	return s_failures == 0U ? 0 : 1;
}
