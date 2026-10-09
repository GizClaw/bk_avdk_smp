#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef enum {
	PM_TRANSITION_NONE = 0,
	PM_TRANSITION_FIRST_LIVE,
	PM_TRANSITION_LAST_FREE,
} pm_transition_kind_t;

typedef struct {
	pm_transition_kind_t kind;
	uint64_t generation;
} pm_transition_t;

typedef struct {
	uint32_t stack_live_count;
	uint32_t ordinary_live_count;
	uint32_t shared_used_count;
	uint32_t fixed_used_count;
	uint64_t hold_generation;
	uint64_t next_generation;
	bool provisional_hold;
	bool power_down_magic;
	bool fatal;
	bool locked_out;
	bool heap_locked;
	bool ordinary_initialized;
	bool stack_initialized;
	bool ordinary_sentinel;
	bool stack_sentinel;
	uint32_t ordinary_free_nodes;
	uint32_t stack_free_nodes;
	uint32_t ordinary_free_bytes;
	uint32_t stack_free_bytes;
	uint32_t ordinary_min_free;
	uint32_t stack_min_free;
	uint32_t dmb_count;
	uint32_t pm_calls_in_heap_lock;
	uint32_t power_on_calls;
	uint32_t power_on_shared_count;
	uint32_t power_on_fixed_count;
	uint32_t power_on_live_count;
	uint32_t power_on_dmb_count;
	uint32_t magic_rechecks;
	uint32_t fatal_hook_calls;
	uint32_t bk_assert_calls;
	uint32_t extra_lock_calls;
	uint32_t metadata_accesses;
} pm_model_t;

typedef struct {
	uint32_t stack_live_count;
	uint32_t ordinary_live_count;
	uint32_t shared_used_count;
	uint32_t fixed_used_count;
	uint64_t hold_generation;
	uint64_t next_generation;
	bool provisional_hold;
	bool power_down_magic;
} accounting_snapshot_t;

static unsigned s_checks;
static unsigned s_failures;

static void expect_true(bool condition, const char *name)
{
	s_checks++;
	if (!condition) {
		s_failures++;
		fprintf(stderr, "PM-05A:MODEL:CHECK-FAIL %s\n", name);
	}
}

static accounting_snapshot_t snapshot(const pm_model_t *model)
{
	const accounting_snapshot_t result = {
		.stack_live_count = model->stack_live_count,
		.ordinary_live_count = model->ordinary_live_count,
		.shared_used_count = model->shared_used_count,
		.fixed_used_count = model->fixed_used_count,
		.hold_generation = model->hold_generation,
		.next_generation = model->next_generation,
		.provisional_hold = model->provisional_hold,
		.power_down_magic = model->power_down_magic,
	};

	return result;
}

static bool snapshot_equal(
	const accounting_snapshot_t *left,
	const accounting_snapshot_t *right)
{
	return (left->stack_live_count == right->stack_live_count)
		&& (left->ordinary_live_count == right->ordinary_live_count)
		&& (left->shared_used_count == right->shared_used_count)
		&& (left->fixed_used_count == right->fixed_used_count)
		&& (left->hold_generation == right->hold_generation)
		&& (left->next_generation == right->next_generation)
		&& (left->provisional_hold == right->provisional_hold)
		&& (left->power_down_magic == right->power_down_magic);
}

static bool snapshot_matches(
	const accounting_snapshot_t *expected,
	const pm_model_t *model)
{
	const accounting_snapshot_t current = snapshot(model);

	return snapshot_equal(expected, &current);
}

static int stack_alloc_commit(
	pm_model_t *model,
	bool metadata_commit_succeeds,
	pm_transition_t *transition)
{
	accounting_snapshot_t before = snapshot(model);

	transition->kind = PM_TRANSITION_NONE;
	transition->generation = 0U;
	if (!metadata_commit_succeeds
		|| model->stack_live_count == UINT32_MAX
		|| model->shared_used_count == UINT32_MAX
		|| model->fixed_used_count == UINT32_MAX) {
		return snapshot_matches(&before, model) ? -1 : -2;
	}
	model->stack_live_count++;
	model->shared_used_count++;
	model->fixed_used_count++;
	model->dmb_count++;
	if (model->stack_live_count == 1U) {
		transition->kind = PM_TRANSITION_FIRST_LIVE;
	}
	return 0;
}

static int stack_free_commit(
	pm_model_t *model,
	bool exact_free_succeeds,
	pm_transition_t *transition)
{
	accounting_snapshot_t before = snapshot(model);

	transition->kind = PM_TRANSITION_NONE;
	transition->generation = 0U;
	if (model->locked_out
		|| !exact_free_succeeds
		|| model->stack_live_count == 0U
		|| model->shared_used_count == 0U
		|| model->fixed_used_count == 0U
		|| (model->stack_live_count == 1U
			&& model->hold_generation != 0U)) {
		return snapshot_matches(&before, model) ? -1 : -2;
	}
	if ((model->stack_live_count == 1U)
		&& (model->next_generation == UINT64_MAX)) {
		model->fatal = true;
		model->locked_out = true;
		model->fatal_hook_calls++;
		return snapshot_matches(&before, model) ? -1 : -2;
	}
	model->stack_live_count--;
	if (model->stack_live_count == 0U) {
		model->next_generation++;
		model->hold_generation = model->next_generation;
		transition->kind = PM_TRANSITION_LAST_FREE;
		transition->generation = model->hold_generation;
	} else {
		model->shared_used_count--;
		model->fixed_used_count--;
	}
	model->dmb_count++;
	return 0;
}

static int final_hold_ack_commit(pm_model_t *model, uint64_t generation)
{
	accounting_snapshot_t before = snapshot(model);

	if (generation == 0U
		|| generation != model->hold_generation
		|| model->shared_used_count <= model->stack_live_count
		|| model->fixed_used_count <= model->stack_live_count) {
		return snapshot_matches(&before, model) ? -1 : -2;
	}
	model->shared_used_count--;
	model->fixed_used_count--;
	model->hold_generation = 0U;
	model->dmb_count++;
	return 0;
}

static int rebuild_both_heaps_locked(pm_model_t *model)
{
	if (!model->heap_locked) {
		return -1;
	}
	if (!model->power_down_magic) {
		return 0;
	}
	if (model->stack_live_count != 0U
		|| model->ordinary_live_count != 0U
		|| model->shared_used_count
			!= (model->provisional_hold ? 1U : 0U)
		|| model->fixed_used_count
			!= (model->provisional_hold ? 1U : 0U)
		|| model->hold_generation != 0U) {
		model->fatal = true;
		model->fatal_hook_calls++;
		return -1;
	}
	model->ordinary_initialized = true;
	model->stack_initialized = true;
	model->ordinary_sentinel = true;
	model->stack_sentinel = true;
	model->ordinary_free_nodes = 1U;
	model->stack_free_nodes = 1U;
	model->ordinary_free_bytes = 3072U;
	model->stack_free_bytes = 4096U;
	model->ordinary_min_free = model->ordinary_free_bytes;
	model->stack_min_free = model->stack_free_bytes;
	model->shared_used_count = model->provisional_hold ? 1U : 0U;
	model->fixed_used_count = model->provisional_hold ? 1U : 0U;
	model->dmb_count++;
	model->power_down_magic = false;
	return 0;
}

static int ordinary_alloc_first(pm_model_t *model)
{
	int result;

	model->heap_locked = true;
	result = rebuild_both_heaps_locked(model);
	if (result == 0) {
		model->ordinary_live_count++;
		model->shared_used_count++;
		model->fixed_used_count++;
		model->dmb_count++;
	}
	model->heap_locked = false;
	return result;
}

static int stack_alloc_first_after_power_down(
	pm_model_t *model,
	bool flip_magic_before_first_lock,
	bool power_on_succeeds,
	bool metadata_commit_succeeds,
	bool invariant_failure,
	pm_transition_t *transition)
{
	int result;

	if (flip_magic_before_first_lock) {
		model->power_down_magic = true;
	}
	model->heap_locked = true;
	model->magic_rechecks++;
	if (model->power_down_magic) {
		model->shared_used_count++;
		model->fixed_used_count++;
		model->provisional_hold = true;
		model->dmb_count++;
		model->heap_locked = false;

		model->power_on_calls++;
		model->power_on_shared_count = model->shared_used_count;
		model->power_on_fixed_count = model->fixed_used_count;
		model->power_on_live_count = model->stack_live_count;
		model->power_on_dmb_count = model->dmb_count;
		if (!power_on_succeeds) {
			model->shared_used_count--;
			model->fixed_used_count--;
			model->provisional_hold = false;
			model->dmb_count++;
			transition->kind = PM_TRANSITION_NONE;
			transition->generation = 0U;
			return -1;
		}

		model->heap_locked = true;
		model->magic_rechecks++;
	}
	result = rebuild_both_heaps_locked(model);
	if ((result == 0) && metadata_commit_succeeds) {
		if (model->provisional_hold) {
			model->stack_live_count++;
			model->provisional_hold = false;
			model->dmb_count++;
			transition->kind = PM_TRANSITION_FIRST_LIVE;
			transition->generation = 0U;
		} else {
			result = stack_alloc_commit(model, true, transition);
		}
	} else if (invariant_failure) {
		model->fatal = true;
		model->fatal_hook_calls++;
		result = -1;
	} else if (model->provisional_hold) {
		model->shared_used_count--;
		model->fixed_used_count--;
		model->provisional_hold = false;
		model->dmb_count++;
		result = -1;
	} else if (!metadata_commit_succeeds) {
		result = -1;
	}
	model->heap_locked = false;
	return result;
}

static int feature_off_ordinary_recover(pm_model_t *model)
{
	if (!model->power_down_magic
		|| model->shared_used_count != 0U
		|| model->fixed_used_count != 0U) {
		return -1;
	}
	model->heap_locked = true;
	model->ordinary_initialized = true;
	model->ordinary_sentinel = true;
	model->ordinary_free_nodes = 1U;
	model->ordinary_free_bytes = 3072U;
	model->ordinary_min_free = model->ordinary_free_bytes;
	model->power_down_magic = false;
	model->heap_locked = false;
	return 0;
}

static void test_accounting_transitions(void)
{
	pm_model_t model = {0};
	pm_transition_t transition;
	uint64_t first_generation;
	uint64_t second_generation;

	expect_true(
		stack_alloc_commit(&model, true, &transition) == 0
			&& transition.kind == PM_TRANSITION_FIRST_LIVE
			&& transition.generation == 0U
			&& model.stack_live_count == 1U
			&& model.shared_used_count == 1U
			&& model.fixed_used_count == 1U,
		"first committed allocation increments all counts and reports transition");
	expect_true(
		stack_alloc_commit(&model, true, &transition) == 0
			&& transition.kind == PM_TRANSITION_NONE
			&& transition.generation == 0U
			&& model.stack_live_count == 2U
			&& model.shared_used_count == 2U
			&& model.fixed_used_count == 2U,
		"second allocation increments exactly without first-live transition");
	expect_true(
		stack_free_commit(&model, true, &transition) == 0
			&& transition.kind == PM_TRANSITION_NONE
			&& transition.generation == 0U
			&& model.stack_live_count == 1U
			&& model.shared_used_count == 1U
			&& model.fixed_used_count == 1U,
		"non-final free decrements all counts");
	expect_true(
		stack_free_commit(&model, true, &transition) == 0
			&& transition.kind == PM_TRANSITION_LAST_FREE
			&& transition.generation != 0U
			&& model.stack_live_count == 0U
			&& model.shared_used_count == 1U
			&& model.fixed_used_count == 1U
			&& model.hold_generation == transition.generation,
		"last free retains one hold and reports a nonzero generation");
	first_generation = transition.generation;

	expect_true(
		stack_alloc_commit(&model, true, &transition) == 0
			&& model.stack_live_count == 1U
			&& model.shared_used_count == 2U
			&& model.fixed_used_count == 2U
			&& model.hold_generation == first_generation,
		"new live allocation coexists with the old generation hold");
	expect_true(
		final_hold_ack_commit(&model, first_generation) == 0
			&& model.stack_live_count == 1U
			&& model.shared_used_count == 1U
			&& model.fixed_used_count == 1U
			&& model.hold_generation == 0U,
		"ACK commit releases only the old hold while new live exists");
	accounting_snapshot_t after_ack = snapshot(&model);
	expect_true(
		final_hold_ack_commit(&model, first_generation) != 0
			&& snapshot_matches(&after_ack, &model),
		"duplicate ACK commit is failure-atomic");

	expect_true(
		stack_free_commit(&model, true, &transition) == 0
			&& transition.kind == PM_TRANSITION_LAST_FREE
			&& transition.generation != 0U
			&& transition.generation != first_generation,
		"next last-free emits a distinct nonzero generation");
	second_generation = transition.generation;
	accounting_snapshot_t second_hold = snapshot(&model);
	expect_true(
		final_hold_ack_commit(&model, first_generation) != 0
			&& snapshot_matches(&second_hold, &model),
		"cross-generation stale ACK is failure-atomic");
	expect_true(
		final_hold_ack_commit(&model, second_generation) == 0
			&& model.shared_used_count == 0U
			&& model.fixed_used_count == 0U
			&& model.hold_generation == 0U,
		"current generation ACK releases its matching hold");
	expect_true(model.dmb_count == 8U, "every successful accounting commit publishes DMB");
}

static void test_generation_exhaustion(void)
{
	pm_model_t model = {
		.next_generation = UINT64_MAX - 2U,
	};
	pm_transition_t transition;

	(void)stack_alloc_commit(&model, true, &transition);
	expect_true(
		stack_free_commit(&model, true, &transition) == 0
			&& transition.generation == UINT64_MAX - 1U,
		"uint64 generation remains nonzero and monotonic near exhaustion");
	const uint64_t penultimate_generation = transition.generation;
	(void)final_hold_ack_commit(&model, transition.generation);
	(void)stack_alloc_commit(&model, true, &transition);
	expect_true(
		stack_free_commit(&model, true, &transition) == 0
			&& transition.generation == UINT64_MAX
			&& transition.generation > penultimate_generation,
		"UINT64_MAX is emitted once without wrapping to reserved zero");
	(void)final_hold_ack_commit(&model, transition.generation);
	(void)stack_alloc_commit(&model, true, &transition);
	const accounting_snapshot_t before_exhaustion = snapshot(&model);
	expect_true(
		stack_free_commit(&model, true, &transition) != 0
			&& transition.kind == PM_TRANSITION_NONE
			&& transition.generation == 0U
			&& snapshot_matches(&before_exhaustion, &model)
			&& model.fatal
			&& model.locked_out
			&& model.fatal_hook_calls == 1U,
		"post-UINT64_MAX generation request fails closed before mutation");
	expect_true(
		stack_free_commit(&model, true, &transition) != 0
			&& transition.generation == 0U
			&& snapshot_matches(&before_exhaustion, &model)
			&& model.fatal_hook_calls == 1U,
		"generation lockout forbids reuse and repeated fatal mutation");
}

static void test_failure_atomicity(void)
{
	pm_model_t model = {0};
	pm_transition_t transition;
	accounting_snapshot_t before = snapshot(&model);

	expect_true(
		stack_alloc_commit(&model, false, &transition) != 0
			&& transition.kind == PM_TRANSITION_NONE
			&& transition.generation == 0U
			&& snapshot_matches(&before, &model),
		"failed allocation commit leaves accounting and transition unchanged");
	model.stack_live_count = 1U;
	model.shared_used_count = 1U;
	model.fixed_used_count = 1U;
	before = snapshot(&model);
	expect_true(
		stack_free_commit(&model, false, &transition) != 0
			&& transition.kind == PM_TRANSITION_NONE
			&& transition.generation == 0U
			&& snapshot_matches(&before, &model),
		"failed exact free leaves accounting and transition unchanged");
	model.shared_used_count = 0U;
	model.fixed_used_count = 0U;
	before = snapshot(&model);
	expect_true(
		stack_free_commit(&model, true, &transition) != 0
			&& snapshot_matches(&before, &model),
		"underflow is rejected without partial mutation");

	memset(&model, 0, sizeof(model));
	model.stack_live_count = 1U;
	model.shared_used_count = 1U;
	model.fixed_used_count = 1U;
	model.hold_generation = 9U;
	before = snapshot(&model);
	expect_true(
		final_hold_ack_commit(&model, 9U) != 0
			&& snapshot_matches(&before, &model),
		"ACK rejects live one hold one count one without mutation");
}

static void prepare_old_hold(pm_model_t *model, uint64_t *generation)
{
	pm_transition_t transition;

	(void)stack_alloc_commit(model, true, &transition);
	(void)stack_free_commit(model, true, &transition);
	*generation = transition.generation;
}

static void test_ack_allocation_race(void)
{
	pm_model_t allocate_then_ack = {0};
	pm_model_t ack_then_allocate;
	pm_transition_t transition;
	uint64_t generation;

	prepare_old_hold(&allocate_then_ack, &generation);
	ack_then_allocate = allocate_then_ack;

	expect_true(
		stack_alloc_commit(&allocate_then_ack, true, &transition) == 0
			&& final_hold_ack_commit(
				&allocate_then_ack, generation) == 0,
		"allocation-then-ACK interleaving succeeds");
	expect_true(
		final_hold_ack_commit(&ack_then_allocate, generation) == 0
			&& stack_alloc_commit(
				&ack_then_allocate, true, &transition) == 0,
		"ACK-then-allocation interleaving succeeds");
	const accounting_snapshot_t first_result = snapshot(&allocate_then_ack);
	const accounting_snapshot_t second_result = snapshot(&ack_then_allocate);
	expect_true(
		allocate_then_ack.stack_live_count == 1U
			&& allocate_then_ack.shared_used_count == 1U
			&& allocate_then_ack.fixed_used_count == 1U
			&& allocate_then_ack.hold_generation == 0U
			&& snapshot_equal(&first_result, &second_result),
		"both deterministic race orders preserve only the new live reference");
}

static void test_provisional_power_on_protocol(void)
{
	pm_model_t model = {
		.power_down_magic = true,
	};
	pm_transition_t transition;

	expect_true(
		stack_alloc_first_after_power_down(
			&model, false, true, true, false, &transition) == 0
			&& model.power_on_calls == 1U
			&& model.power_on_shared_count == 1U
			&& model.power_on_fixed_count == 1U
			&& model.power_on_live_count == 0U
			&& model.power_on_dmb_count != 0U
			&& model.magic_rechecks == 2U
			&& model.stack_live_count == 1U
			&& model.shared_used_count == 1U
			&& model.fixed_used_count == 1U
			&& !model.provisional_hold,
		"provisional hold precedes unlocked power-on and converts without double count");

	memset(&model, 0, sizeof(model));
	model.power_down_magic = true;
	expect_true(
		stack_alloc_first_after_power_down(
			&model, false, false, true, false, &transition) != 0
			&& model.power_on_calls == 1U
			&& model.shared_used_count == 0U
			&& model.fixed_used_count == 0U
			&& !model.provisional_hold
			&& model.dmb_count == 2U,
		"power-on failure atomically cleans a provisional hold");

	memset(&model, 0, sizeof(model));
	expect_true(
		stack_alloc_first_after_power_down(
			&model, true, true, true, false, &transition) == 0
			&& model.power_on_calls == 1U
			&& model.magic_rechecks == 2U
			&& model.stack_live_count == 1U
			&& model.shared_used_count == 1U
			&& model.fixed_used_count == 1U,
		"magic flip after observation and before lock remains safe");

	memset(&model, 0, sizeof(model));
	model.power_down_magic = true;
	expect_true(
		stack_alloc_first_after_power_down(
			&model, false, true, false, false, &transition) != 0
			&& model.shared_used_count == 0U
			&& model.fixed_used_count == 0U
			&& !model.provisional_hold,
		"post-power allocation failure cleans the provisional hold");

	memset(&model, 0, sizeof(model));
	model.power_down_magic = true;
	expect_true(
		stack_alloc_first_after_power_down(
			&model, false, true, false, true, &transition) != 0
			&& model.fatal
			&& model.fatal_hook_calls == 1U
			&& model.provisional_hold,
		"rebuild or cache invariant failure reaches fatal with conservative hold");
}

static int shared_allocation_begin_locked(pm_model_t *model)
{
	if (!model->heap_locked || model->provisional_hold) {
		return -1;
	}
	if (model->shared_used_count != 0U) {
		model->metadata_accesses++;
		return 0;
	}
	model->provisional_hold = true;
	model->shared_used_count++;
	model->fixed_used_count++;
	model->dmb_count++;
	model->heap_locked = false;
	return 1;
}

static void shared_leader_abort_locked(pm_model_t *model)
{
	model->heap_locked = true;
	model->shared_used_count--;
	model->fixed_used_count--;
	model->provisional_hold = false;
	model->dmb_count++;
}

static int shared_mutator_preflight_locked(pm_model_t *model)
{
	if (!model->heap_locked || model->provisional_hold) {
		return -1;
	}
	model->metadata_accesses++;
	return 0;
}

static void test_shared_single_inflight_protocol(void)
{
	pm_model_t model = {.heap_locked = true};

	expect_true(
		shared_allocation_begin_locked(&model) == 1
			&& model.provisional_hold
			&& !model.heap_locked
			&& model.shared_used_count == 1U
			&& model.fixed_used_count == 1U,
		"ordinary and stack share one leader provisional hold");
	model.heap_locked = true;
	const accounting_snapshot_t inflight = snapshot(&model);
	expect_true(
		shared_allocation_begin_locked(&model) != 0
			&& snapshot_matches(&inflight, &model)
			&& model.metadata_accesses == 0U,
		"second inflight allocation retries without metadata access");
	const accounting_snapshot_t before_stale_mutators = snapshot(&model);
	expect_true(
		shared_mutator_preflight_locked(&model) != 0
			&& snapshot_matches(&before_stale_mutators, &model)
			&& model.metadata_accesses == 0U,
		"stale free fails before rebuild or cache during leader power inflight");
	expect_true(
		shared_mutator_preflight_locked(&model) != 0
			&& snapshot_matches(&before_stale_mutators, &model)
			&& model.metadata_accesses == 0U,
		"stale hold ACK fails before rebuild or cache during leader power inflight");
	shared_leader_abort_locked(&model);
	expect_true(
		!model.provisional_hold
			&& model.shared_used_count == 0U
			&& model.fixed_used_count == 0U,
		"leader power failure or OOM clears the provisional hold");

	memset(&model, 0, sizeof(model));
	model.heap_locked = true;
	model.shared_used_count = 1U;
	model.fixed_used_count = 1U;
	expect_true(
		shared_allocation_begin_locked(&model) == 0
			&& model.heap_locked
			&& model.power_on_calls == 0U
			&& model.metadata_accesses == 1U,
		"used allocation stays in the original lock without power window");
}

static int classify_allocation_failure(
	pm_model_t *model,
	bool metadata_corrupt)
{
	if (metadata_corrupt) {
		model->fatal = true;
		model->fatal_hook_calls++;
		return -2;
	}
	return -1;
}

static void test_oom_and_corruption_classification(void)
{
	pm_model_t model = {
		.stack_live_count = 2U,
		.shared_used_count = 2U,
		.fixed_used_count = 2U,
	};
	const accounting_snapshot_t fragmented = snapshot(&model);

	expect_true(
		classify_allocation_failure(&model, false) == -1
			&& !model.fatal
			&& snapshot_matches(&fragmented, &model),
		"fragmented OOM is a normal failure with unchanged accounting");
	expect_true(
		classify_allocation_failure(&model, true) == -2
			&& model.fatal
			&& model.fatal_hook_calls == 1U,
		"allocator metadata corruption enters crash-safe fatal");
}

static void test_power_down_rebuild(void)
{
	pm_model_t model = {
		.stack_live_count = 1U,
		.shared_used_count = 1U,
		.fixed_used_count = 1U,
		.power_down_magic = true,
		.heap_locked = true,
	};

	expect_true(
		rebuild_both_heaps_locked(&model) != 0
			&& model.fatal
			&& model.power_down_magic
			&& model.fatal_hook_calls == 1U
			&& model.bk_assert_calls == 0U
			&& model.extra_lock_calls == 0U,
		"power-down magic with a live stack is fatal before magic clear");

	memset(&model, 0, sizeof(model));
	model.ordinary_live_count = 1U;
	model.shared_used_count = 1U;
	model.fixed_used_count = 1U;
	model.power_down_magic = true;
	model.heap_locked = true;
	expect_true(
		rebuild_both_heaps_locked(&model) != 0
			&& model.fatal
			&& model.power_down_magic,
		"power-down magic with any ordinary reference is fatal");

	memset(&model, 0, sizeof(model));
	model.hold_generation = 7U;
	model.next_generation = 7U;
	model.shared_used_count = 1U;
	model.fixed_used_count = 1U;
	model.power_down_magic = true;
	model.heap_locked = true;
	expect_true(
		rebuild_both_heaps_locked(&model) != 0
			&& model.fatal
			&& model.power_down_magic,
		"power-down magic with only the retained PM hold is fatal");

	memset(&model, 0, sizeof(model));
	model.power_down_magic = true;
	expect_true(
		ordinary_alloc_first(&model) == 0
			&& model.ordinary_initialized
			&& model.stack_initialized
			&& model.ordinary_sentinel
			&& model.stack_sentinel
			&& model.ordinary_free_nodes == 1U
			&& model.stack_free_nodes == 1U,
		"ordinary-first rebuilds both heaps");
	expect_true(
		model.ordinary_min_free == model.ordinary_free_bytes
			&& model.stack_min_free == model.stack_free_bytes,
		"dual rebuild restores minimum-free accounting");
	expect_true(
		!model.power_down_magic
			&& model.ordinary_live_count == 1U
			&& model.shared_used_count == 1U
			&& model.fixed_used_count == 1U,
		"magic clears only before the new ordinary allocation is committed");
	expect_true(
		model.pm_calls_in_heap_lock == 0U,
		"dual rebuild and accounting make no PM or mailbox call under heap lock");

	memset(&model, 0, sizeof(model));
	model.power_down_magic = true;
	expect_true(
		feature_off_ordinary_recover(&model) == 0
			&& model.ordinary_initialized
			&& model.ordinary_sentinel
			&& !model.stack_initialized
			&& !model.power_down_magic,
		"feature-off power-down recovery preserves ordinary baseline");
}

int main(void)
{
	test_accounting_transitions();
	test_generation_exhaustion();
	test_failure_atomicity();
	test_ack_allocation_race();
	test_provisional_power_on_protocol();
	test_shared_single_inflight_protocol();
	test_oom_and_corruption_classification();
	test_power_down_rebuild();

	if (s_failures != 0U) {
		printf(
			"PM-05A:MODEL:FAIL checks=%u failures=%u\n",
			s_checks,
			s_failures);
		return 1;
	}
	printf(
		"PM-05A:MODEL:PASS checks=%u failures=0\n",
		s_checks);
	return 0;
}
