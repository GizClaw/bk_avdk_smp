#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define main cachealloc_existing_main
#include "allocator_contract_test.c"
#undef main

typedef struct {
	int kind;
	uint64_t generation;
} expected_transition_t;

typedef struct {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	bool allocation_done;
	void *payload;
	expected_transition_t transition;
	uint64_t hold_generation;
	int allocation_result;
	int ack_result;
} race_context_t;

typedef struct {
	bool stack_leader;
	void *payload;
	expected_transition_t transition;
	int result;
} inflight_context_t;

typedef struct {
	size_t ordinary_free;
	size_t stack_free;
	uint32_t shared_count;
	uint32_t fixed_count;
	uint32_t live_count;
	uint64_t hold_generation;
	unsigned int cache_calls;
	unsigned int fatal_calls;
	bool provisional_hold;
	uint32_t power_down_magic;
} pm_metadata_snapshot_t;

static void *s_fragment_payloads[2048];

static int actual_alloc(
	size_t requested_size,
	void **payload,
	expected_transition_t *transition)
{
#if defined(PSRAM_STACK_PM_GENERATION_API_VERSION)
	psram_stack_pm_transition_t actual = {0};
	const int result = psram_stack_alloc_with_transition(
		requested_size, payload, &actual);

	transition->kind = actual.kind;
	transition->generation = actual.generation;
	return result;
#else
	psram_stack_pm_transition_t actual = PSRAM_STACK_PM_TRANSITION_NONE;
	const int result = psram_stack_alloc_with_transition(
		requested_size, payload, &actual);

	transition->kind = actual;
	transition->generation = 0U;
	return result;
#endif
}

static int actual_free(
	void *payload,
	size_t requested_size,
	expected_transition_t *transition)
{
#if defined(PSRAM_STACK_PM_GENERATION_API_VERSION)
	psram_stack_pm_transition_t actual = {0};
	const int result = psram_stack_free_exact_with_transition(
		payload, requested_size, &actual);

	transition->kind = actual.kind;
	transition->generation = actual.generation;
	return result;
#else
	psram_stack_pm_transition_t actual = PSRAM_STACK_PM_TRANSITION_NONE;
	const int result = psram_stack_free_exact_with_transition(
		payload, requested_size, &actual);

	transition->kind = actual;
	transition->generation = 0U;
	return result;
#endif
}

static int actual_ack_commit(uint64_t generation)
{
#if defined(PSRAM_STACK_PM_GENERATION_API_VERSION)
	return psram_stack_pm_hold_release_commit(generation);
#else
	(void)generation;
	return psram_stack_pm_hold_release_commit();
#endif
}

static int direct_rebuild(void)
{
	int result;

	HeapEnterCritical();
	result = psram_heaps_rebuild_after_power_down_locked();
	HeapExitCritical();
	return result;
}

static size_t core_cache_line_count(unsigned int core_id, bool dirty)
{
	size_t count = 0U;

	for (size_t line = 0U; line < CACHE_LINE_COUNT; line++) {
		if (s_model.core[core_id][line].valid
			&& (!dirty || s_model.core[core_id][line].dirty)) {
			count++;
		}
	}
	return count;
}

static pm_metadata_snapshot_t pm_metadata_snapshot(void)
{
	const pm_metadata_snapshot_t result = {
		.ordinary_free = psram_xFreeBytesRemaining,
		.stack_free = s_psram_stack_free_bytes,
		.shared_count = s_psram_used_count,
		.fixed_count = FIXED_ADDR_PSRAM_USDE_COUNT,
		.live_count = s_psram_stack_live_count,
		.hold_generation = s_psram_stack_pm_hold_generation,
		.cache_calls = s_cache_call_count,
		.fatal_calls = s_pm_fatal_hook_calls,
		.provisional_hold = s_psram_stack_pm_provisional_hold,
		.power_down_magic = FIXED_ADDR_PSRAM_POWER_DOWN,
	};

	return result;
}

static bool pm_metadata_matches(
	const pm_metadata_snapshot_t *left,
	const pm_metadata_snapshot_t *right)
{
	return (left->ordinary_free == right->ordinary_free)
		&& (left->stack_free == right->stack_free)
		&& (left->shared_count == right->shared_count)
		&& (left->fixed_count == right->fixed_count)
		&& (left->live_count == right->live_count)
		&& (left->hold_generation == right->hold_generation)
		&& (left->cache_calls == right->cache_calls)
		&& (left->fatal_calls == right->fatal_calls)
		&& (left->provisional_hold == right->provisional_hold)
		&& (left->power_down_magic == right->power_down_magic);
}

static void *inflight_leader_worker(void *argument)
{
	inflight_context_t *context = argument;

	if (context->stack_leader) {
		context->result = actual_alloc(
			32U, &context->payload, &context->transition);
	} else {
		context->payload =
			psram_malloc_cm(__func__, __LINE__, 32U, 0);
		context->result = context->payload == NULL ? -1 : 0;
	}
	return NULL;
}

static bool wait_for_power_block(void)
{
	struct timespec deadline;

	if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
		return false;
	}
	deadline.tv_sec++;
	(void)pthread_mutex_lock(&s_power_block_mutex);
	while (!s_power_block_entered) {
		if (pthread_cond_timedwait(
				&s_power_block_condition,
				&s_power_block_mutex,
				&deadline) != 0) {
			(void)pthread_mutex_unlock(&s_power_block_mutex);
			return false;
		}
	}
	(void)pthread_mutex_unlock(&s_power_block_mutex);
	return true;
}

static void release_power_block(void)
{
	(void)pthread_mutex_lock(&s_power_block_mutex);
	s_power_block_release = true;
	(void)pthread_cond_broadcast(&s_power_block_condition);
	(void)pthread_mutex_unlock(&s_power_block_mutex);
}

static void *allocation_race_worker(void *argument)
{
	race_context_t *context = argument;

	context->allocation_result = actual_alloc(
		64U, &context->payload, &context->transition);
	(void)pthread_mutex_lock(&context->mutex);
	context->allocation_done = true;
	(void)pthread_cond_broadcast(&context->condition);
	(void)pthread_mutex_unlock(&context->mutex);
	return NULL;
}

static void *ack_race_worker(void *argument)
{
	race_context_t *context = argument;

	(void)pthread_mutex_lock(&context->mutex);
	while (!context->allocation_done) {
		(void)pthread_cond_wait(&context->condition, &context->mutex);
	}
	(void)pthread_mutex_unlock(&context->mutex);
	context->ack_result = actual_ack_commit(context->hold_generation);
	return NULL;
}

static unsigned test_generation_contract(void)
{
	unsigned failures = 0U;
	void *payload = NULL;
	expected_transition_t transition = {0};

	allocator_reset();
	if ((actual_alloc(32U, &payload, &transition) != 0)
		|| (actual_free(payload, 32U, &transition) != 0)
		|| (transition.kind != PSRAM_STACK_PM_TRANSITION_LAST_FREE)
		|| (transition.generation == 0U)) {
		fprintf(stderr, "PM-05A:ACTUAL:MISSING last-free-nonzero-generation\n");
		failures++;
	}
	const uint64_t old_generation = transition.generation;
	if (actual_ack_commit(old_generation) != 0) {
		failures++;
	}
	const uint32_t shared_after_ack = s_psram_used_count;
	const uint32_t fixed_after_ack = FIXED_ADDR_PSRAM_USDE_COUNT;
	if ((actual_ack_commit(old_generation) == 0)
		|| (s_psram_used_count != shared_after_ack)
		|| (FIXED_ADDR_PSRAM_USDE_COUNT != fixed_after_ack)) {
		fprintf(stderr, "PM-05A:ACTUAL:MISSING duplicate-ack-atomic-reject\n");
		failures++;
	}

	if ((actual_alloc(32U, &payload, &transition) != 0)
		|| (actual_free(payload, 32U, &transition) != 0)
		|| (transition.generation == 0U)
		|| (transition.generation == old_generation)) {
		fprintf(stderr, "PM-05A:ACTUAL:MISSING distinct-next-generation\n");
		failures++;
	}
	const uint64_t new_generation = transition.generation;
	const uint32_t shared_before_stale = s_psram_used_count;
	const uint32_t fixed_before_stale = FIXED_ADDR_PSRAM_USDE_COUNT;
	if ((actual_ack_commit(old_generation) == 0)
		|| (s_psram_used_count != shared_before_stale)
		|| (FIXED_ADDR_PSRAM_USDE_COUNT != fixed_before_stale)) {
		fprintf(stderr, "PM-05A:ACTUAL:MISSING cross-generation-ack-reject\n");
		failures++;
	}
	if ((new_generation != 0U) && (actual_ack_commit(new_generation) != 0)) {
		failures++;
	}

	allocator_reset();
	s_psram_stack_live_count = 1U;
	s_psram_stack_pm_hold_generation = 17U;
	s_psram_used_count = 1U;
	FIXED_ADDR_PSRAM_USDE_COUNT = 1U;
	if ((actual_ack_commit(17U) == 0)
		|| (s_psram_stack_live_count != 1U)
		|| (s_psram_stack_pm_hold_generation != 17U)
		|| (s_psram_used_count != 1U)
		|| (FIXED_ADDR_PSRAM_USDE_COUNT != 1U)) {
		fprintf(
			stderr,
			"PM-05A:ACTUAL:MISSING ack-requires-count-above-live\n");
		failures++;
	}
	return failures;
}

static unsigned test_two_thread_ack_allocation_race(void)
{
	unsigned failures = 0U;
	void *old_payload = NULL;
	expected_transition_t old_transition = {0};
	pthread_t allocation_thread;
	pthread_t ack_thread;
	race_context_t context = {
		.mutex = PTHREAD_MUTEX_INITIALIZER,
		.condition = PTHREAD_COND_INITIALIZER,
	};

	allocator_reset();
	if ((actual_alloc(32U, &old_payload, &old_transition) != 0)
		|| (actual_free(old_payload, 32U, &old_transition) != 0)) {
		return 1U;
	}
	context.hold_generation = old_transition.generation;
	if ((pthread_create(
			&allocation_thread,
			NULL,
			allocation_race_worker,
			&context) != 0)
		|| (pthread_create(
			&ack_thread,
			NULL,
			ack_race_worker,
			&context) != 0)) {
		return 1U;
	}
	(void)pthread_join(allocation_thread, NULL);
	(void)pthread_join(ack_thread, NULL);
	(void)pthread_cond_destroy(&context.condition);
	(void)pthread_mutex_destroy(&context.mutex);
	/* ADR-004: a pending last-free hold rejects cacheable-stack allocation, so
	 * the racing allocation must fail closed and the ACK must release exactly
	 * that hold, leaving no residual reference. */
	if ((context.allocation_result == 0)
		|| (context.ack_result != 0)
		|| (s_psram_stack_live_count != 0U)
		|| (s_psram_stack_pm_hold_generation != 0U)
		|| (s_psram_used_count != 0U)
		|| (FIXED_ADDR_PSRAM_USDE_COUNT != 0U)) {
		fprintf(
			stderr,
			"PM-05A:ACTUAL:MISSING ack-allocation-race-hold-blocks-alloc\n");
		failures++;
	}

	/* Releasing the hold must reopen allocation; otherwise the block above
	 * would be indistinguishable from a permanent lockout. */
	void *new_payload = NULL;
	expected_transition_t new_transition = {0};
	if ((actual_alloc(64U, &new_payload, &new_transition) != 0)
		|| (s_psram_stack_live_count != 1U)
		|| (s_psram_used_count != 1U)
		|| (FIXED_ADDR_PSRAM_USDE_COUNT != 1U)) {
		fprintf(
			stderr,
			"PM-05A:ACTUAL:MISSING ack-allocation-race-alloc-after-release\n");
		failures++;
	}
	return failures;
}

static unsigned test_shared_single_inflight_guard(void)
{
	unsigned failures = 0U;

	for (unsigned int stack_leader = 0U; stack_leader < 2U; stack_leader++) {
		pthread_t leader_thread;
		inflight_context_t leader = {
			.stack_leader = stack_leader != 0U,
		};
		void *follower_payload = NULL;
		expected_transition_t follower_transition = {0};
		int follower_result;

		allocator_reset();
		FIXED_ADDR_PSRAM_POWER_DOWN = PM_PSRAM_POWER_DOWN_MAGIC;
		s_power_block_enabled = true;
		if ((pthread_create(
				&leader_thread,
				NULL,
				inflight_leader_worker,
				&leader) != 0)
			|| !wait_for_power_block()) {
			release_power_block();
			return failures + 1U;
		}
		const pm_metadata_snapshot_t before = pm_metadata_snapshot();
		if (leader.stack_leader) {
			follower_payload =
				psram_malloc_cm(__func__, __LINE__, 32U, 0);
			follower_result = follower_payload == NULL ? -1 : 0;
		} else {
			follower_result = actual_alloc(
				32U, &follower_payload, &follower_transition);
		}
		const pm_metadata_snapshot_t after = pm_metadata_snapshot();
		if ((follower_result == 0)
			|| !pm_metadata_matches(&before, &after)) {
			const char *missing = leader.stack_leader
				? "single-inflight-stack-leader"
				: "single-inflight-ordinary-leader";
			fprintf(
				stderr,
				"PM-05A:ACTUAL:MISSING %s\n",
				missing);
			failures++;
		}
		release_power_block();
		(void)pthread_join(leader_thread, NULL);
		s_power_block_enabled = false;
		if (leader.result != 0) {
			failures++;
		} else if (leader.stack_leader) {
			follower_payload =
				psram_malloc_cm(__func__, __LINE__, 32U, 0);
			if (follower_payload == NULL) {
				failures++;
			}
		} else if (actual_alloc(
				32U, &follower_payload, &follower_transition) != 0) {
			failures++;
		}
	}
	return failures;
}

static unsigned run_inflight_stale_mutator(bool test_ack)
{
	void *stale_payload = NULL;
	expected_transition_t stale_transition = {0};
	inflight_context_t leader = {.stack_leader = true};
	pthread_t leader_thread;

	allocator_reset();
	if (actual_alloc(32U, &stale_payload, &stale_transition) != 0) {
		fprintf(stderr, "PM-05A:ACTUAL:HARNESS-FAIL stale-setup-alloc\n");
		return 1U;
	}
	if (actual_free(stale_payload, 32U, &stale_transition) != 0) {
		fprintf(stderr, "PM-05A:ACTUAL:HARNESS-FAIL stale-setup-free\n");
		return 1U;
	}
	const uint64_t stale_generation = stale_transition.generation;
	if (actual_ack_commit(stale_generation) != 0) {
		fprintf(stderr, "PM-05A:ACTUAL:HARNESS-FAIL stale-setup-ack\n");
		return 1U;
	}
	FIXED_ADDR_PSRAM_POWER_DOWN = PM_PSRAM_POWER_DOWN_MAGIC;
	s_power_on_call_count = 0U;
	s_power_block_entered = false;
	s_power_block_release = false;
	s_power_block_enabled = true;
	if ((pthread_create(
			&leader_thread,
			NULL,
			inflight_leader_worker,
			&leader) != 0)
		|| !wait_for_power_block()) {
		release_power_block();
		return 1U;
	}
	const pm_metadata_snapshot_t before = pm_metadata_snapshot();
	const int stale_result = test_ack
		? actual_ack_commit(stale_generation)
		: actual_free(stale_payload, 32U, &stale_transition);
	const pm_metadata_snapshot_t after = pm_metadata_snapshot();
	release_power_block();
	(void)pthread_join(leader_thread, NULL);
	s_power_block_enabled = false;
	if ((stale_result == 0)
		|| !pm_metadata_matches(&before, &after)
		|| (leader.result != 0)) {
		fprintf(
			stderr,
			"PM-05A:ACTUAL:MISSING %s\n",
			test_ack
				? "inflight-stale-ack-preflight"
				: "inflight-stale-free-preflight");
		return 1U;
	}
	return 0U;
}

static unsigned test_inflight_rejects_stale_mutators(void)
{
	return run_inflight_stale_mutator(false)
		+ run_inflight_stale_mutator(true);
}

static unsigned test_used_count_direct_allocation(void)
{
	unsigned failures = 0U;
	void *stack = NULL;
	void *ordinary;
	expected_transition_t transition = {0};

	allocator_reset();
	FIXED_ADDR_PSRAM_POWER_DOWN = PM_PSRAM_POWER_DOWN_MAGIC;
	if (actual_alloc(32U, &stack, &transition) != 0) {
		return 1U;
	}
	s_power_on_call_count = 0U;
	const unsigned int ordinary_enters = s_heap_enter_count;
	const unsigned int ordinary_exits = s_heap_exit_count;
	ordinary = psram_malloc_cm(__func__, __LINE__, 32U, 0);
	if ((ordinary == NULL)
		|| (s_power_on_call_count != 0U)
		|| (s_heap_enter_count != ordinary_enters + 1U)
		|| (s_heap_exit_count != ordinary_exits + 1U)) {
		fprintf(stderr, "PM-05A:ACTUAL:MISSING used-direct-ordinary-one-lock\n");
		failures++;
	}
	const unsigned int stack_enters = s_heap_enter_count;
	const unsigned int stack_exits = s_heap_exit_count;
	if ((actual_alloc(32U, &stack, &transition) != 0)
		|| (s_power_on_call_count != 0U)
		|| (s_heap_enter_count != stack_enters + 1U)
		|| (s_heap_exit_count != stack_exits + 1U)) {
		fprintf(stderr, "PM-05A:ACTUAL:MISSING used-direct-stack-one-lock\n");
		failures++;
	}
	return failures;
}

static unsigned test_rebuild_entry_paths(void)
{
	unsigned failures = 0U;
	void *first = NULL;
	void *peer = NULL;
	void *rebuilt = NULL;
	void *ordinary;
	void *stack = NULL;
	expected_transition_t transition = {0};
	expected_transition_t first_free = {0};

	allocator_reset();
	s_core_id = 0U;
	if (actual_alloc(32U, &first, &transition) != 0) {
		return 1U;
	}
	s_core_id = 1U;
	if ((actual_alloc(64U, &peer, &transition) != 0)
		|| (actual_free(peer, 64U, &transition) != 0)) {
		return 1U;
	}
	const size_t peer_valid_before_rebuild = core_cache_line_count(1U, false);
	const size_t peer_dirty_before_all_free = core_cache_line_count(1U, true);
	s_core_id = 0U;
	if ((peer_dirty_before_all_free != 0U)
		|| (peer_valid_before_rebuild == 0U)
		|| (actual_free(first, 32U, &first_free) != 0)
		|| (actual_ack_commit(first_free.generation) != 0)) {
		fprintf(stderr, "PM-05A:ACTUAL:MISSING peer-metadata-clean-before-all-free\n");
		failures++;
	}
	FIXED_ADDR_PSRAM_POWER_DOWN = PM_PSRAM_POWER_DOWN_MAGIC;
	if (actual_alloc(32U, &rebuilt, &transition) != 0) {
		failures++;
	}
	s_core_id = 1U;
	if ((actual_alloc(32U, &peer, &transition) != 0)
		|| !topology_valid()
		|| (FIXED_ADDR_PSRAM_POWER_DOWN != 0U)
		|| (core_cache_line_count(1U, true) != 0U)) {
		fprintf(stderr, "PM-05A:ACTUAL:MISSING peer-stale-clean-refill-after-rebuild\n");
		failures++;
	}

	allocator_reset();
	FIXED_ADDR_PSRAM_POWER_DOWN = PM_PSRAM_POWER_DOWN_MAGIC;
	ordinary = psram_malloc_cm(__func__, __LINE__, 32U, 0);
	if ((ordinary == NULL)
		|| (s_psram_stack_end == NULL)
		|| !topology_valid()) {
		fprintf(stderr, "PM-05A:ACTUAL:MISSING ordinary-first-dual-rebuild\n");
		failures++;
	}
	if (ordinary != NULL) {
		vPortFree(ordinary);
	}

	allocator_reset();
	FIXED_ADDR_PSRAM_POWER_DOWN = PM_PSRAM_POWER_DOWN_MAGIC;
	if ((actual_alloc(32U, &stack, &transition) != 0)
		|| (s_power_on_call_count != 1U)
		|| (s_power_on_inside_heap_lock != 0U)
		|| (FIXED_ADDR_PSRAM_POWER_DOWN != 0U)) {
		fprintf(
			stderr,
			"PM-05A:ACTUAL:MISSING stack-first-power-on-outside-lock-recheck\n");
		failures++;
	}
	return failures;
}

static unsigned test_provisional_power_on_protocol(void)
{
	unsigned failures = 0U;
	void *payload = NULL;
	expected_transition_t transition = {0};

	allocator_reset();
	FIXED_ADDR_PSRAM_POWER_DOWN = PM_PSRAM_POWER_DOWN_MAGIC;
	if ((actual_alloc(32U, &payload, &transition) != 0)
		|| (s_power_on_call_count != 1U)
		|| (s_power_on_inside_heap_lock != 0U)
		|| (s_power_on_shared_count != 1U)
		|| (s_power_on_fixed_count != 1U)
		|| (s_power_on_live_count != 0U)
		|| (s_power_on_dmb_count == 0U)
		|| (s_psram_stack_live_count != 1U)
		|| (s_psram_used_count != 1U)
		|| (FIXED_ADDR_PSRAM_USDE_COUNT != 1U)) {
		fprintf(
			stderr,
			"PM-05A:ACTUAL:MISSING provisional-hold-before-power-on\n");
		failures++;
	}

	allocator_reset();
	s_flip_magic_on_next_heap_enter = true;
	s_flip_magic_value = PM_PSRAM_POWER_DOWN_MAGIC;
	if ((actual_alloc(32U, &payload, &transition) != 0)
		|| (s_power_on_call_count != 1U)
		|| (s_power_on_shared_count != 1U)
		|| (s_power_on_fixed_count != 1U)
		|| (s_power_on_dmb_count == 0U)
		|| (s_psram_stack_live_count != 1U)
		|| (s_psram_used_count != 1U)
		|| (FIXED_ADDR_PSRAM_USDE_COUNT != 1U)) {
		fprintf(stderr, "PM-05A:ACTUAL:MISSING magic-toctou-recheck\n");
		failures++;
	}

	allocator_reset();
	FIXED_ADDR_PSRAM_POWER_DOWN = PM_PSRAM_POWER_DOWN_MAGIC;
	s_power_on_result = BK_FAIL;
	if ((actual_alloc(32U, &payload, &transition) == 0)
		|| (s_power_on_call_count != 1U)
		|| (s_cache_call_count != s_power_on_cache_call_count)
		|| (s_psram_used_count != 0U)
		|| (FIXED_ADDR_PSRAM_USDE_COUNT != 0U)) {
		fprintf(
			stderr,
			"PM-05A:ACTUAL:MISSING power-fail-cleans-without-metadata\n");
		failures++;
	}

	allocator_reset();
	FIXED_ADDR_PSRAM_POWER_DOWN = PM_PSRAM_POWER_DOWN_MAGIC;
	s_power_on_result = BK_FAIL;
	payload = psram_malloc_cm(__func__, __LINE__, 32U, 0);
	if ((payload != NULL)
		|| (s_power_on_call_count != 1U)
		|| (s_cache_call_count != s_power_on_cache_call_count)
		|| (s_psram_used_count != 0U)
		|| (FIXED_ADDR_PSRAM_USDE_COUNT != 0U)) {
		fprintf(stderr, "PM-05A:ACTUAL:MISSING ordinary-leader-power-fail-clean\n");
		failures++;
	}

	allocator_reset();
	FIXED_ADDR_PSRAM_POWER_DOWN = PM_PSRAM_POWER_DOWN_MAGIC;
	s_fail_cache_call = 1U;
	if ((actual_alloc(32U, &payload, &transition) == 0)
		|| (s_pm_fatal_hook_calls != 1U)
		|| (s_psram_used_count != FIXED_ADDR_PSRAM_USDE_COUNT)
		|| (s_psram_used_count > 1U)) {
		fprintf(stderr, "PM-05A:ACTUAL:MISSING rebuild-cache-failure-fatal\n");
		failures++;
	}

	allocator_reset();
	FIXED_ADDR_PSRAM_POWER_DOWN = PM_PSRAM_POWER_DOWN_MAGIC;
	if ((actual_alloc(HEAP_SIZE, &payload, &transition) == 0)
		|| (s_psram_used_count != 0U)
		|| (FIXED_ADDR_PSRAM_USDE_COUNT != 0U)) {
		fprintf(stderr, "PM-05A:ACTUAL:MISSING oom-cleans-provisional-hold\n");
		failures++;
	}

	allocator_reset();
	FIXED_ADDR_PSRAM_POWER_DOWN = PM_PSRAM_POWER_DOWN_MAGIC;
	payload = psram_malloc_cm(__func__, __LINE__, HEAP_SIZE, 0);
	if ((payload != NULL)
		|| (s_psram_used_count != 0U)
		|| (FIXED_ADDR_PSRAM_USDE_COUNT != 0U)
		|| (s_pm_fatal_hook_calls != 0U)) {
		fprintf(stderr, "PM-05A:ACTUAL:MISSING ordinary-leader-oom-cleans-hold\n");
		failures++;
	}
	return failures;
}

static unsigned test_fragmentation_and_corruption(void)
{
	unsigned failures = 0U;
	expected_transition_t transition = {0};
	size_t allocated = 0U;

	allocator_reset();
	if (actual_alloc(
			128U, &s_fragment_payloads[0], &transition) != 0) {
		return 1U;
	}
	const size_t free_before_large = s_psram_stack_free_bytes;
	if (actual_alloc(
			256U, &s_fragment_payloads[1], &transition) != 0) {
		return 1U;
	}
	const size_t large_cost =
		free_before_large - s_psram_stack_free_bytes;
	allocator_reset();
	memset(s_fragment_payloads, 0, sizeof(s_fragment_payloads));
	while ((allocated < 2048U)
		&& ((allocated == 0U)
			|| (s_psram_stack_free_bytes >= large_cost))
		&& (actual_alloc(
			128U, &s_fragment_payloads[allocated], &transition) == 0)) {
		allocated++;
	}
	for (size_t index = 1U; index + 1U < allocated; index += 2U) {
		if (actual_free(
				s_fragment_payloads[index], 128U, &transition) != 0) {
			return 1U;
		}
	}
	const uint32_t shared_before_oom = s_psram_used_count;
	const uint32_t fixed_before_oom = FIXED_ADDR_PSRAM_USDE_COUNT;
	void *large = NULL;
	if ((actual_alloc(256U, &large, &transition) == 0)
		|| (s_pm_fatal_hook_calls != 0U)
		|| (s_psram_used_count != shared_before_oom)
		|| (FIXED_ADDR_PSRAM_USDE_COUNT != fixed_before_oom)) {
		fprintf(
			stderr,
			"PM-05A:ACTUAL:MISSING fragmented-oom-nonfatal "
			"blocks=%zu allocated=%u fatal=%u shared=%u/%u fixed=%u/%u\n",
			allocated,
			large != NULL,
			s_pm_fatal_hook_calls,
			(unsigned int)s_psram_used_count,
			(unsigned int)shared_before_oom,
			(unsigned int)FIXED_ADDR_PSRAM_USDE_COUNT,
			(unsigned int)fixed_before_oom);
		failures++;
	}

	allocator_reset();
	void *corrupt = NULL;
	if (actual_alloc(32U, &corrupt, &transition) != 0) {
		return failures + 1U;
	}
	PsramStackBlock_t *block =
		(PsramStackBlock_t *)((uint8_t *)corrupt - PSRAM_STACK_HEADER_SIZE);
	block->block.pxNextFreeBlock = &block->block;
	if (cache_stack_metadata_clean_aligned(
			block, PSRAM_STACK_HEADER_SIZE) != 0) {
		return failures + 1U;
	}
	void *after_corruption = NULL;
	(void)actual_alloc(32U, &after_corruption, &transition);
	if (s_pm_fatal_hook_calls != 1U) {
		fprintf(stderr, "PM-05A:ACTUAL:MISSING corruption-is-fatal\n");
		failures++;
	}
	return failures;
}

static unsigned test_magic_reference_fatal(void)
{
	void *payload = NULL;
	expected_transition_t transition = {0};

	allocator_reset();
	if (actual_alloc(32U, &payload, &transition) != 0) {
		return 1U;
	}
	const unsigned asserts_before = s_assert_count;
	FIXED_ADDR_PSRAM_POWER_DOWN = PM_PSRAM_POWER_DOWN_MAGIC;
	(void)direct_rebuild();
	const bool passed = (s_pm_fatal_hook_calls == 1U)
		&& (s_pm_fatal_hook_lock_depth == 1U)
		&& (s_assert_count == asserts_before)
		&& (s_extra_lock_calls_in_fatal == 0U);
	if (!passed) {
		fprintf(stderr, "PM-05A:ACTUAL:MISSING magic-ref-config-wdt-fatal\n");
	}
	return !passed;
}

int main(void)
{
	const unsigned generation_failures = test_generation_contract();
	const unsigned race_failures = test_two_thread_ack_allocation_race();
	const unsigned inflight_failures = test_shared_single_inflight_guard();
	const unsigned stale_failures = test_inflight_rejects_stale_mutators();
	const unsigned direct_failures = test_used_count_direct_allocation();
	const unsigned rebuild_failures = test_rebuild_entry_paths();
	const unsigned power_failures = test_provisional_power_on_protocol();
	const unsigned integrity_failures = test_fragmentation_and_corruption();
	const unsigned fatal_failures = test_magic_reference_fatal();
	const unsigned failures = generation_failures + race_failures
		+ inflight_failures + stale_failures + direct_failures + rebuild_failures
		+ power_failures + integrity_failures + fatal_failures;

	if (failures != 0U) {
		printf(
			"PM-05A:ACTUAL:FAIL failures=%u generation=%u race=%u "
			"inflight=%u stale=%u direct=%u rebuild=%u power=%u "
			"integrity=%u fatal=%u\n",
			failures,
			generation_failures,
			race_failures,
			inflight_failures,
			stale_failures,
			direct_failures,
			rebuild_failures,
			power_failures,
			integrity_failures,
			fatal_failures);
		return 1;
	}
	printf(
		"PM-05A:ACTUAL:PASS generation=1 stale=1 cross=1 "
		"ordinary_first=1 stack_first=1 peer_refill=1 race=1 "
		"single_inflight=1 stale_preflight=1 used_direct=1 "
		"provisional=1 toctou=1 "
		"fragmented_oom=1 corruption_fatal=1 fatal=1\n");
	return 0;
}
