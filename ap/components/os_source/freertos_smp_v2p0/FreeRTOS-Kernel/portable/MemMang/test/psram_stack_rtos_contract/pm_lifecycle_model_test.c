#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
	VOTE_ACTIVE = 0,
	VOTE_RELEASED = 1,
} vote_state_t;

typedef struct {
	uint32_t live;
	uint64_t hold_generation;
	uint64_t next_generation;
	uint32_t facade_calls;
	uint32_t active_calls;
	uint32_t release_calls;
	uint32_t publications;
	uint32_t exact_frees;
	uint32_t tcb_frees;
	uint32_t fatal_calls;
	uint32_t metadata_calls;
	uint32_t tls_deinit_calls;
	bool facade_inflight;
	bool baseline_ready;
	bool lockout;
	bool kernel_locked;
	bool heap_locked;
	bool irq_enabled;
	bool task_context;
	bool tcb_allocated;
	bool tls_initialized;
	bool scheduler_visible;
	bool vote_active;
} pm_model_t;

static unsigned s_checks;
static unsigned s_failures;

static void expect_true(bool condition, const char *name)
{
	s_checks++;
	if (!condition) {
		s_failures++;
		fprintf(stderr, "PM-05B:MODEL:CHECK-FAIL %s\n", name);
	}
}

static int facade_vote(
	pm_model_t *model,
	vote_state_t state,
	bool succeeds)
{
	if (!model->task_context
		|| !model->irq_enabled
		|| model->kernel_locked
		|| model->heap_locked
		|| model->facade_inflight) {
		return -1;
	}
	model->facade_inflight = true;
	model->facade_calls++;
	model->active_calls += state == VOTE_ACTIVE;
	model->release_calls += state == VOTE_RELEASED;
	if (succeeds) {
		model->vote_active = state == VOTE_ACTIVE;
	}
	model->facade_inflight = false;
	return succeeds ? 0 : -1;
}

static int lazy_resync(pm_model_t *model, bool release_succeeds)
{
	if (model->baseline_ready) {
		return 0;
	}
	if (facade_vote(model, VOTE_RELEASED, release_succeeds) != 0) {
		model->lockout = true;
		return -1;
	}
	model->baseline_ready = true;
	return 0;
}

static uint64_t exact_free(pm_model_t *model, bool succeeds)
{
	if (model->kernel_locked || !model->tcb_allocated) {
		return 0U;
	}
	model->metadata_calls++;
	if (!succeeds) {
		model->fatal_calls++;
		return 0U;
	}
	model->exact_frees++;
	model->live--;
	model->next_generation++;
	model->hold_generation = model->next_generation;
	return model->hold_generation;
}

static void free_tcb_once(pm_model_t *model)
{
	if (model->tcb_allocated) {
		if (model->tls_initialized) {
			model->tls_initialized = false;
			model->tls_deinit_calls++;
		}
		model->tcb_allocated = false;
		model->tcb_frees++;
	}
}

static int compensate_create(
	pm_model_t *model,
	bool free_succeeds,
	bool release_succeeds)
{
	const uint64_t token = exact_free(model, free_succeeds);

	if (token == 0U) {
		return -2;
	}
	if (facade_vote(model, VOTE_RELEASED, release_succeeds) != 0) {
		model->lockout = true;
		free_tcb_once(model);
		return -1;
	}
	model->hold_generation = 0U;
	free_tcb_once(model);
	return -1;
}

static int create_first_live(
	pm_model_t *model,
	bool baseline_succeeds,
	bool active_succeeds,
	bool free_succeeds,
	bool compensation_succeeds)
{
	if (model->lockout || model->facade_inflight
		|| model->hold_generation != 0U) {
		return -1;
	}
	if (lazy_resync(model, baseline_succeeds) != 0) {
		return -1;
	}
	model->tcb_allocated = true;
	model->tls_initialized = true;
	model->live++;
	model->kernel_locked = true;
	model->kernel_locked = false;
	if (facade_vote(model, VOTE_ACTIVE, active_succeeds) != 0) {
		return compensate_create(
			model, free_succeeds, compensation_succeeds);
	}
	model->kernel_locked = true;
	model->scheduler_visible = true;
	model->publications++;
	model->kernel_locked = false;
	return 0;
}

static int delete_last(
	pm_model_t *model,
	bool release_succeeds)
{
	model->kernel_locked = true;
	model->scheduler_visible = false;
	model->kernel_locked = false;
	const uint64_t token = exact_free(model, true);

	if (token == 0U) {
		return -2;
	}
	if (facade_vote(model, VOTE_RELEASED, release_succeeds) == 0) {
		model->hold_generation = 0U;
	} else {
		model->lockout = true;
	}
	free_tcb_once(model);
	return release_succeeds ? 0 : -1;
}

static pm_model_t initial_model(void)
{
	const pm_model_t model = {
		.irq_enabled = true,
		.task_context = true,
	};

	return model;
}

static void test_create_success_and_resync(void)
{
	pm_model_t model = initial_model();

	expect_true(
		create_first_live(&model, true, true, true, true) == 0,
		"first create succeeds");
	expect_true(
		model.release_calls == 1U
			&& model.active_calls == 1U
			&& model.publications == 1U,
		"lazy baseline release precedes first active publication");
	expect_true(
		model.baseline_ready
			&& model.scheduler_visible
			&& !model.kernel_locked
			&& !model.heap_locked,
		"ready publication occurs only after unlocked ACKs");

	model = initial_model();
	expect_true(
		create_first_live(&model, false, true, true, true) != 0
			&& model.lockout
			&& model.publications == 0U
			&& model.active_calls == 0U,
		"failed lazy resync prevents active vote and publication");
}

static void test_create_failure_paths(void)
{
	pm_model_t model = initial_model();

	expect_true(
		create_first_live(&model, true, false, true, true) != 0
			&& model.publications == 0U
			&& model.exact_frees == 1U
			&& model.release_calls == 2U
			&& model.hold_generation == 0U
			&& model.tcb_frees == 1U
			&& model.tls_deinit_calls == 1U,
		"active NACK rolls back and commits compensated last-free hold");

	model = initial_model();
	expect_true(
		create_first_live(&model, true, false, true, false) != 0
			&& model.lockout
			&& model.hold_generation != 0U
			&& model.publications == 0U
			&& model.tcb_frees == 1U,
		"failed compensation retains hold and locks out creation");
	expect_true(
		create_first_live(&model, true, true, true, true) != 0
			&& model.publications == 0U,
		"lockout rejects later create");

	model = initial_model();
	expect_true(
		create_first_live(&model, true, false, false, true) == -2
			&& model.fatal_calls == 1U
			&& model.publications == 0U,
		"rollback exact-free failure reaches WDT fatal");
}

static void test_concurrency_and_lock_context(void)
{
	pm_model_t model = initial_model();

	model.facade_inflight = true;
	expect_true(
		create_first_live(&model, true, true, true, true) != 0
			&& model.metadata_calls == 0U
			&& model.facade_calls == 0U,
		"concurrent first-live follower retries without transaction");

	model = initial_model();
	model.kernel_locked = true;
	expect_true(
		facade_vote(&model, VOTE_ACTIVE, true) != 0,
		"facade rejects kernel-lock context");
	model.kernel_locked = false;
	model.heap_locked = true;
	expect_true(
		facade_vote(&model, VOTE_RELEASED, true) != 0,
		"facade rejects heap-lock context");
	model.heap_locked = false;
	model.irq_enabled = false;
	expect_true(
		facade_vote(&model, VOTE_ACTIVE, true) != 0,
		"facade rejects IRQ-disabled context");
}

static void test_delete_and_generation(void)
{
	pm_model_t model = initial_model();

	model.baseline_ready = true;
	model.live = 1U;
	model.tcb_allocated = true;
	model.tls_initialized = true;
	model.scheduler_visible = true;
	expect_true(
		delete_last(&model, true) == 0
			&& !model.scheduler_visible
			&& model.exact_frees == 1U
			&& model.release_calls == 1U
			&& model.hold_generation == 0U
			&& model.tcb_frees == 1U
			&& model.tls_deinit_calls == 1U,
		"delete removes ownership, exact-frees, ACKs, commits, then frees TCB");

	model = initial_model();
	model.baseline_ready = true;
	model.live = 1U;
	model.tcb_allocated = true;
	model.tls_initialized = true;
	model.scheduler_visible = true;
	expect_true(
		delete_last(&model, false) != 0
			&& model.lockout
			&& model.hold_generation != 0U
			&& model.tcb_frees == 1U,
		"delete NACK retains hold and records lockout");
	free_tcb_once(&model);
	expect_true(
		model.tcb_frees == 1U,
		"delete failure never double-frees TCB");

	const uint64_t old_token = model.hold_generation;
	expect_true(
		old_token != 0U
			&& create_first_live(&model, true, true, true, true) != 0
			&& model.live == 0U,
		"pending last-free hold blocks new live allocation");
}

static void test_heap_linearized_interleavings(void)
{
	pm_model_t model = initial_model();

	expect_true(
		create_first_live(&model, true, true, true, true) == 0
			&& delete_last(&model, true) == 0
			&& !model.facade_inflight
			&& !model.kernel_locked
			&& !model.heap_locked,
		"shared transactions converge without task-owned participant");

	model = initial_model();
	model.baseline_ready = true;
	model.live = 1U;
	model.tcb_allocated = true;
	model.tls_initialized = true;
	model.scheduler_visible = true;
	model.vote_active = true;
	expect_true(
		delete_last(&model, true) == 0
			&& create_first_live(&model, true, true, true, true) == 0
			&& model.live == 1U
			&& model.vote_active,
		"heap-linearized last-free then first-live ends active with one live task");

	model = initial_model();
	model.baseline_ready = true;
	model.live = 1U;
	model.vote_active = true;
	model.live++;
	model.live--;
	expect_true(
		model.live == 1U
			&& model.vote_active
			&& model.release_calls == 0U,
		"heap-linearized first-live arrival makes competing delete non-final");
}

int main(void)
{
	test_create_success_and_resync();
	test_create_failure_paths();
	test_concurrency_and_lock_context();
	test_delete_and_generation();
	test_heap_linearized_interleavings();

	if (s_failures != 0U) {
		printf(
			"PM-05B:MODEL:FAIL checks=%u failures=%u\n",
			s_checks,
			s_failures);
		return 1;
	}
	printf(
		"PM-05B:MODEL:PASS checks=%u failures=0\n",
		s_checks);
	return 0;
}
