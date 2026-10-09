#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#define main allocator_contract_main_unused
#include "../psram_stack_contract/allocator_contract_test.c"
#undef main

static unsigned s_checks;
static unsigned s_failures;

static void pm05b_expect(bool condition, const char *name)
{
	s_checks++;
	if (!condition) {
		s_failures++;
		fprintf(stderr, "PM-05B:ACTUAL:CHECK-FAIL %s\n", name);
	}
}

int main(void)
{
	void *first = NULL;
	void *new_live = NULL;
	psram_stack_pm_transition_t first_transition = {0};
	psram_stack_pm_transition_t last_transition = {0};
	psram_stack_pm_transition_t new_transition = {0};

	allocator_reset();
	pm05b_expect(
		psram_stack_alloc_with_transition(
			64U, &first, &first_transition) == PSRAM_STACK_STATUS_OK
			&& first_transition.kind
				== PSRAM_STACK_PM_TRANSITION_FIRST_LIVE,
		"actual first allocation returns first-live transition");
	pm05b_expect(
		psram_stack_free_exact_with_transition(
			first, 64U, &last_transition) == PSRAM_STACK_STATUS_OK
			&& last_transition.kind
				== PSRAM_STACK_PM_TRANSITION_LAST_FREE
			&& last_transition.generation != 0U,
		"actual last free returns nonzero generation token");
	const uint64_t old_generation = last_transition.generation;
	const size_t held_used = s_psram_used_count;
	const uint32_t held_fixed = FIXED_ADDR_PSRAM_USDE_COUNT;
	pm05b_expect(
		psram_stack_alloc_with_transition(
			64U, &new_live, &new_transition) != PSRAM_STACK_STATUS_OK
			&& new_live == NULL
			&& new_transition.kind == PSRAM_STACK_PM_TRANSITION_NONE
			&& s_psram_stack_live_count == 0U
			&& s_psram_used_count == held_used
			&& FIXED_ADDR_PSRAM_USDE_COUNT == held_fixed,
		"pending last-free hold blocks new allocation without mutation");
	pm05b_expect(
		psram_stack_pm_hold_release_commit(old_generation)
				== PSRAM_STACK_STATUS_OK
			&& s_psram_stack_live_count == 0U
			&& s_psram_used_count == 0U
			&& FIXED_ADDR_PSRAM_USDE_COUNT == 0U
			&& s_psram_stack_pm_hold_generation == 0U,
		"exact generation commit releases pending hold");
	pm05b_expect(
		psram_stack_pm_hold_release_commit(old_generation)
				!= PSRAM_STACK_STATUS_OK
			&& s_psram_stack_live_count == 0U
			&& s_psram_used_count == 0U,
		"duplicate generation cannot mutate released state");

	if (s_failures != 0U) {
		printf(
			"PM-05B:ACTUAL:FAIL checks=%u failures=%u\n",
			s_checks,
			s_failures);
		return 1;
	}
	printf(
		"PM-05B:ACTUAL:PASS checks=%u failures=0 generation=%" PRIu64 "\n",
		s_checks,
		old_generation);
	return 0;
}
