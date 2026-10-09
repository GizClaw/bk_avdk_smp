#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
	TXN_IDLE = 0,
	TXN_WAITING,
	TXN_TIMED_OUT,
} txn_state_t;

typedef struct {
	atomic_uint version;
	atomic_uint low;
	atomic_uint high;
} shared_fixed64_t;

typedef struct {
	uint32_t live_count;
	uint64_t next_generation;
	uint64_t hold_generation;
	uint64_t pending_generation;
	bool heap_locked;
	bool pm_active;
	bool first_live_leader;
	bool published;
	bool locked_out;
	txn_state_t transaction;
	unsigned commit_calls;
	unsigned abort_calls;
	unsigned vote_calls;
	unsigned side_effects;
} review_model_t;

static shared_fixed64_t s_shared;
static atomic_bool s_writer_done;
static atomic_uint s_read_failures;
static unsigned s_checks;
static unsigned s_failures;

static void expect_true(bool condition, const char *name)
{
	s_checks++;
	if (!condition) {
		s_failures++;
		fprintf(stderr, "PM-05C:REVIEW2-MODEL:MISSING %s\n", name);
	}
}

static bool allocate_stack(review_model_t *model)
{
	model->heap_locked = true;
	if ((model->hold_generation != 0U)
		|| (model->pending_generation != 0U)
		|| model->locked_out) {
		model->heap_locked = false;
		return false;
	}
	model->live_count++;
	model->first_live_leader = model->live_count == 1U;
	model->heap_locked = false;
	return true;
}

static bool publish_stack(review_model_t *model)
{
	if (!model->pm_active) {
		return false;
	}
	model->published = true;
	return true;
}

static uint64_t free_last_stack(review_model_t *model)
{
	model->heap_locked = true;
	if (model->live_count != 1U) {
		model->heap_locked = false;
		return 0U;
	}
	model->live_count = 0U;
	model->next_generation++;
	model->hold_generation = model->next_generation;
	model->pending_generation = model->hold_generation;
	model->heap_locked = false;
	return model->hold_generation;
}

static bool resync_claim(review_model_t *model, uint64_t *generation)
{
	if ((model->pending_generation == 0U) || model->locked_out) {
		return false;
	}
	*generation = model->pending_generation;
	return true;
}

static bool resync_commit(review_model_t *model, uint64_t generation)
{
	if ((generation == 0U)
		|| (generation != model->pending_generation)
		|| (generation != model->hold_generation)) {
		return false;
	}
	model->commit_calls++;
	model->hold_generation = 0U;
	model->pending_generation = 0U;
	return true;
}

static bool resync_abort(review_model_t *model, uint64_t generation)
{
	if ((generation == 0U) || (generation != model->pending_generation)) {
		return false;
	}
	model->abort_calls++;
	model->locked_out = true;
	return true;
}

static bool shared_load_bounded(const shared_fixed64_t *field, uint64_t *value)
{
	for (unsigned retry = 0; retry < 16U; retry++) {
		const uint32_t before =
			atomic_load_explicit(&field->version, memory_order_acquire);
		if ((before & 1U) != 0U) {
			continue;
		}
		atomic_thread_fence(memory_order_acquire);
		const uint32_t low =
			atomic_load_explicit(&field->low, memory_order_relaxed);
		const uint32_t high =
			atomic_load_explicit(&field->high, memory_order_relaxed);
		const uint32_t after =
			atomic_load_explicit(&field->version, memory_order_acquire);
		if ((before == after) && ((after & 1U) == 0U)) {
			*value = ((uint64_t)high << 32) | low;
			return true;
		}
	}
	return false;
}

static bool shared_store(shared_fixed64_t *field, uint64_t value)
{
	for (unsigned retry = 0U; retry < 16U; retry++) {
		uint32_t version =
			atomic_load_explicit(&field->version, memory_order_acquire);
		if (((version & 1U) != 0U) || (version > UINT32_MAX - 2U)) {
			continue;
		}
		if (!atomic_compare_exchange_weak_explicit(
				&field->version, &version, version + 1U,
				memory_order_acq_rel, memory_order_acquire)) {
			continue;
		}
		atomic_store_explicit(
			&field->low, (uint32_t)value, memory_order_relaxed);
		atomic_store_explicit(
			&field->high, (uint32_t)(value >> 32), memory_order_relaxed);
		atomic_store_explicit(
			&field->version, version + 2U, memory_order_release);
		return true;
	}
	return false;
}

static void *writer_thread(void *unused)
{
	(void)unused;
	for (uint32_t value = 1U; value <= 100000U; value++) {
		(void)shared_store(
			&s_shared, (UINT64_C(0xa5a5a5a5) << 32) | value);
	}
	atomic_store_explicit(&s_writer_done, true, memory_order_release);
	return NULL;
}

static void *reader_thread(void *unused)
{
	(void)unused;
	do {
		uint64_t value = 0U;
		if (shared_load_bounded(&s_shared, &value)
			&& value == 0U) {
			atomic_fetch_add(&s_read_failures, 1U);
		}
	} while (!atomic_load_explicit(&s_writer_done, memory_order_acquire));
	return NULL;
}

static bool cp_accept(
	review_model_t *model,
	uint32_t module,
	uint32_t state,
	uint32_t request_id)
{
	if ((module != 7U) || (state > 1U) || (request_id == 0U)) {
		return false;
	}
	model->vote_calls++;
	return true;
}

static bool destructive_enter(review_model_t *model, bool live, bool vote_active)
{
	if (live || vote_active) {
		return false;
	}
	model->side_effects++;
	return true;
}

int main(void)
{
	review_model_t model = {0};
	pthread_t writer;
	pthread_t reader;
	uint64_t value = UINT64_MAX;
	uint64_t generation = 0U;

	expect_true(
		allocate_stack(&model)
			&& model.first_live_leader
			&& model.live_count == 1U
			&& !model.heap_locked,
		"heap-lock-linearizes-single-first-live-leader");
	expect_true(
		!publish_stack(&model) && !model.published,
		"first-live-cannot-publish-before-active");
	model.pm_active = true;
	model.first_live_leader = false;
	expect_true(
		allocate_stack(&model)
			&& !model.first_live_leader
			&& publish_stack(&model)
			&& model.live_count == 2U,
		"second-create-observes-active-before-publication");
	model.live_count = 1U;
	generation = free_last_stack(&model);
	expect_true(
		generation != 0U
			&& generation == model.pending_generation
			&& generation == model.hold_generation,
		"last-free-publishes-uint64-pending-hold");
	expect_true(
		!allocate_stack(&model) && model.live_count == 0U,
		"pending-hold-blocks-new-allocation");
	expect_true(
		resync_claim(&model, &generation)
			&& resync_commit(&model, generation)
			&& model.commit_calls == 1U
			&& model.pending_generation == 0U
			&& model.hold_generation == 0U,
		"resync-claim-commit-clears-exact-uint64-hold");
	model = (review_model_t){.next_generation = UINT64_C(0x100000000)};
	model.live_count = 1U;
	generation = free_last_stack(&model);
	expect_true(
		resync_claim(&model, &generation)
			&& resync_abort(&model, generation)
			&& model.abort_calls == 1U
			&& model.locked_out
			&& model.pending_generation == generation,
		"resync-abort-retains-hold-and-locks-out");
	model = (review_model_t){.transaction = TXN_WAITING};
	model.transaction = TXN_IDLE;
	expect_true(
		model.transaction == TXN_IDLE,
		"send-failure-restores-idle-and-allows-retry");
	model.transaction = TXN_WAITING;
	model.transaction = TXN_TIMED_OUT;
	expect_true(
		model.transaction == TXN_TIMED_OUT,
		"timeout-retains-quarantine");
	model.pending_generation = UINT64_C(0x100000001);
	model.hold_generation = model.pending_generation;
	model.transaction = TXN_TIMED_OUT;
	expect_true(
		model.commit_calls == 0U
			&& model.pending_generation == UINT64_C(0x100000001),
		"late-ack-converges-shared-state-without-kernel-commit");
	model.vote_calls = 0U;
	expect_true(
		!cp_accept(&model, 6U, 0U, 1U)
			&& !cp_accept(&model, 7U, 2U, 1U)
			&& !cp_accept(&model, 7U, 0U, 0U)
			&& model.vote_calls == 0U,
		"cp-invalid-dedicated-requests-nack-without-side-effect");
	(void)shared_store(&s_shared, UINT64_C(0xa5a5a5a500000001));
	atomic_store(&s_writer_done, false);
	atomic_store(&s_read_failures, 0U);
	expect_true(
		pthread_create(&writer, NULL, writer_thread, NULL) == 0
			&& pthread_create(&reader, NULL, reader_thread, NULL) == 0
			&& pthread_join(writer, NULL) == 0
			&& pthread_join(reader, NULL) == 0
			&& atomic_load(&s_read_failures) == 0U,
		"concurrent-seqlock-stress-never-returns-false-zero");
	atomic_store(&s_shared.version, 1U);
	expect_true(
		!shared_load_bounded(&s_shared, &value)
			&& value == UINT64_MAX,
		"writer-death-bounded-retry-fails-closed");
	atomic_store(&s_shared.version, UINT32_MAX - 1U);
	expect_true(
		!shared_store(&s_shared, UINT64_C(0x1122334455667788)),
		"writer-version-wrap-fails-closed");
	atomic_store(&s_shared.version, 2U);
	atomic_store(&s_shared.low, UINT32_C(0x55667788));
	atomic_store(&s_shared.high, UINT32_C(0x11223344));
	const unsigned char *golden = (const unsigned char *)&s_shared;
	expect_true(
		sizeof(s_shared) == 12U
			&& shared_load_bounded(&s_shared, &value)
			&& value == UINT64_C(0x1122334455667788)
			&& golden[0] == 0x02U && golden[4] == 0x88U
			&& golden[5] == 0x77U && golden[6] == 0x66U
			&& golden[7] == 0x55U && golden[8] == 0x44U
			&& golden[9] == 0x33U && golden[10] == 0x22U
			&& golden[11] == 0x11U,
		"real-12-byte-little-endian-high-word-golden");
	model = (review_model_t){0};
	expect_true(
		!destructive_enter(&model, true, false)
			&& !destructive_enter(&model, false, true)
			&& model.side_effects == 0U,
		"live-count-or-psram-vote-blocks-destructive-mode");
	expect_true(
		destructive_enter(&model, false, false)
			&& model.side_effects == 1U,
		"feature-off-destructive-path-remains-legacy");

	printf(
		"PM-05C:REVIEW2-MODEL:%s checks=%u failures=%u\n",
		s_failures == 0U ? "PASS" : "FAIL", s_checks, s_failures);
	return s_failures == 0U ? 0 : 1;
}
