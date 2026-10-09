#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef enum {
	EVENT_TCB_ORDINARY_ALLOC,
	EVENT_STACK_ALLOC,
	EVENT_KERNEL_LOCK,
	EVENT_STACK_INIT,
	EVENT_RELEASE,
	EVENT_DSB,
	EVENT_READY_PUBLISH,
	EVENT_KERNEL_UNLOCK,
	EVENT_API_REJECT,
	EVENT_ROLLBACK,
	EVENT_RETURN_FAILURE,
	EVENT_FAIL_STOP,
	EVENT_RECORD_FATAL_REASON,
	EVENT_RECORD_FATAL_CORE,
	EVENT_FATAL_DMB,
	EVENT_PLATFORM_RESET_HOOK,
	EVENT_SET_CRASH_RESET_REASON,
	EVENT_WDT_FORCE_RESET,
	EVENT_LOCAL_IRQ_DISABLE,
	EVENT_FALLBACK_LOOP,
	EVENT_SELECT,
	EVENT_ACQUIRE_CORE0,
	EVENT_ACQUIRE_CORE1,
	EVENT_RESTORE_CORE0,
	EVENT_RESTORE_CORE1,
	EVENT_REMOVE_SCHEDULER,
	EVENT_CLEAR_CURRENT_CORE0,
	EVENT_CLEAR_CURRENT_CORE1,
	EVENT_HEAP_LOCK,
	EVENT_ACQUIRE_FULL_OWNER,
	EVENT_GUARD_VALIDATE,
	EVENT_EXACT_FREE,
	EVENT_HEAP_UNLOCK,
	EVENT_TCB_FREE,
} event_t;

typedef struct {
	uintptr_t addr;
	size_t size;
} owner_span_t;

typedef struct {
	event_t events[64];
	size_t event_count;
	unsigned checks;
	unsigned failures;
	unsigned stack_allocations;
	unsigned ordinary_allocations;
	bool kernel_locked;
	bool heap_locked;
	bool scheduler_visible;
	bool current[2];
	uintptr_t px_stack;
	size_t ul_stack_size;
	bool stack_allocated;
	bool tcb_allocated;
	bool fail_stopped;
	unsigned wdt_reset_calls;
	unsigned select_after_fatal;
	unsigned restore_after_fatal;
} model_t;

static void expect_true(model_t *model, bool condition, const char *message)
{
	model->checks++;
	if (!condition) {
		model->failures++;
		fprintf(stderr, "RTOS-04:MODEL:CHECK-FAIL %s\n", message);
	}
}

static void reset_scenario(model_t *model)
{
	unsigned checks = model->checks;
	unsigned failures = model->failures;
	unsigned wdt_reset_calls = model->wdt_reset_calls;
	unsigned select_after_fatal = model->select_after_fatal;
	unsigned restore_after_fatal = model->restore_after_fatal;

	memset(model, 0, sizeof(*model));
	model->checks = checks;
	model->failures = failures;
	model->wdt_reset_calls = wdt_reset_calls;
	model->select_after_fatal = select_after_fatal;
	model->restore_after_fatal = restore_after_fatal;
}

static void log_event(model_t *model, event_t event)
{
	if (model->fail_stopped && event == EVENT_SELECT) {
		model->select_after_fatal++;
	}
	if (model->fail_stopped
	    && (event == EVENT_RESTORE_CORE0 || event == EVENT_RESTORE_CORE1)) {
		model->restore_after_fatal++;
	}
	if (model->event_count < (sizeof(model->events) / sizeof(model->events[0]))) {
		model->events[model->event_count++] = event;
	}
}

static size_t find_event(const model_t *model, event_t event, size_t start)
{
	for (size_t index = start; index < model->event_count; index++) {
		if (model->events[index] == event) {
			return index;
		}
	}
	return SIZE_MAX;
}

static void kernel_lock(model_t *model)
{
	expect_true(model, !model->kernel_locked, "kernel lock is not recursive");
	expect_true(model, !model->heap_locked, "heap lock never nests into kernel lock");
	model->kernel_locked = true;
	log_event(model, EVENT_KERNEL_LOCK);
}

static void kernel_unlock(model_t *model)
{
	expect_true(model, model->kernel_locked, "kernel unlock has an owner");
	model->kernel_locked = false;
	log_event(model, EVENT_KERNEL_UNLOCK);
}

static bool heap_lock(model_t *model)
{
	if (model->kernel_locked || model->heap_locked) {
		return false;
	}
	model->heap_locked = true;
	log_event(model, EVENT_HEAP_LOCK);
	return true;
}

static void heap_unlock(model_t *model)
{
	expect_true(model, model->heap_locked, "heap unlock has an owner");
	model->heap_locked = false;
	log_event(model, EVENT_HEAP_UNLOCK);
}

static owner_span_t reconstruct_owner_span(const model_t *model)
{
	owner_span_t span = {
		.addr = model->px_stack,
		.size = ((model->ul_stack_size + 31u) & ~(size_t)31u) + 32u,
	};

	return span;
}

static void fail_stop_with_wdt_return(model_t *model, bool wdt_returns)
{
	model->fail_stopped = true;
	log_event(model, EVENT_FAIL_STOP);
	log_event(model, EVENT_RECORD_FATAL_REASON);
	log_event(model, EVENT_RECORD_FATAL_CORE);
	log_event(model, EVENT_FATAL_DMB);
	log_event(model, EVENT_PLATFORM_RESET_HOOK);
	log_event(model, EVENT_SET_CRASH_RESET_REASON);
	model->wdt_reset_calls++;
	log_event(model, EVENT_WDT_FORCE_RESET);
	if (wdt_returns) {
		log_event(model, EVENT_LOCAL_IRQ_DISABLE);
		log_event(model, EVENT_FALLBACK_LOOP);
	}
}

static void fail_stop(model_t *model)
{
	fail_stop_with_wdt_return(model, false);
}

static void create_cacheable_task(
	model_t *model,
	bool release_succeeds,
	bool rollback_succeeds)
{
	expect_true(model, heap_lock(model), "stack allocation takes only the heap lock");
	model->px_stack = 0x60700000u;
	model->ul_stack_size = 0x1001u;
	model->stack_allocated = true;
	model->stack_allocations++;
	log_event(model, EVENT_STACK_ALLOC);
	heap_unlock(model);

	model->tcb_allocated = true;
	log_event(model, EVENT_TCB_ORDINARY_ALLOC);
	kernel_lock(model);
	log_event(model, EVENT_STACK_INIT);
	log_event(model, EVENT_RELEASE);
	if (!release_succeeds) {
		kernel_unlock(model);
		log_event(model, EVENT_ROLLBACK);
		expect_true(model, heap_lock(model), "creation rollback starts unlocked");
		log_event(model, EVENT_EXACT_FREE);
		if (!rollback_succeeds) {
			fail_stop(model);
			return;
		}
		model->stack_allocated = false;
		heap_unlock(model);
		model->tcb_allocated = false;
		log_event(model, EVENT_TCB_FREE);
		log_event(model, EVENT_RETURN_FAILURE);
		return;
	}
	log_event(model, EVENT_DSB);
	model->scheduler_visible = true;
	log_event(model, EVENT_READY_PUBLISH);
	kernel_unlock(model);
}

static void create_from_context(model_t *model, bool in_isr, bool irq_disabled)
{
	if (in_isr || irq_disabled) {
		log_event(model, EVENT_API_REJECT);
		return;
	}
	create_cacheable_task(model, true, true);
}

static void create_legacy_task(model_t *model, bool is_static)
{
	(void)is_static;
	model->ordinary_allocations++;
}

static void switch_task(
	model_t *model,
	unsigned core,
	bool release_succeeds,
	bool acquire_succeeds)
{
	kernel_lock(model);
	log_event(model, EVENT_RELEASE);
	if (!release_succeeds) {
		fail_stop(model);
		return;
	}
	log_event(model, EVENT_DSB);
	log_event(model, EVENT_SELECT);
	model->current[core] = true;
	log_event(model, core == 0u ? EVENT_ACQUIRE_CORE0 : EVENT_ACQUIRE_CORE1);
	if (!acquire_succeeds) {
		fail_stop(model);
		return;
	}
	log_event(model, EVENT_DSB);
	kernel_unlock(model);
	log_event(model, core == 0u ? EVENT_RESTORE_CORE0 : EVENT_RESTORE_CORE1);
}

static void first_task_restore(model_t *model, unsigned core, bool acquire_succeeds)
{
	model->current[core] = true;
	log_event(model, core == 0u ? EVENT_ACQUIRE_CORE0 : EVENT_ACQUIRE_CORE1);
	if (!acquire_succeeds) {
		fail_stop(model);
		return;
	}
	log_event(model, EVENT_DSB);
	log_event(model, core == 0u ? EVENT_RESTORE_CORE0 : EVENT_RESTORE_CORE1);
}

static void delete_task(model_t *model, bool exact_free_succeeds)
{
	kernel_lock(model);
	model->scheduler_visible = false;
	log_event(model, EVENT_REMOVE_SCHEDULER);
	model->current[0] = false;
	log_event(model, EVENT_CLEAR_CURRENT_CORE0);
	model->current[1] = false;
	log_event(model, EVENT_CLEAR_CURRENT_CORE1);
	kernel_unlock(model);

	expect_true(model, heap_lock(model), "exact free starts after kernel unlock");
	log_event(model, EVENT_ACQUIRE_FULL_OWNER);
	log_event(model, EVENT_GUARD_VALIDATE);
	log_event(model, EVENT_EXACT_FREE);
	if (!exact_free_succeeds) {
		fail_stop(model);
		return;
	}
	model->stack_allocated = false;
	heap_unlock(model);
	model->tcb_allocated = false;
	log_event(model, EVENT_TCB_FREE);
}

static void test_creation_and_routes(model_t *model)
{
	size_t begin = model->event_count;
	create_cacheable_task(model, true, true);
	size_t lock = find_event(model, EVENT_KERNEL_LOCK, begin);
	size_t init = find_event(model, EVENT_STACK_INIT, begin);
	size_t release = find_event(model, EVENT_RELEASE, begin);
	size_t publish = find_event(model, EVENT_READY_PUBLISH, begin);
	size_t unlock = find_event(model, EVENT_KERNEL_UNLOCK, begin);
	owner_span_t owner_span = reconstruct_owner_span(model);

	expect_true(model, lock < init && init < release, "creation initializes before release");
	expect_true(model, release < publish && publish < unlock,
		    "release and publication share one kernel-lock window");
	expect_true(model, owner_span.addr == model->px_stack,
		    "owner address is reconstructed from existing pxStack");
	expect_true(model, owner_span.size == 0x1040u,
		    "owner size is reconstructed from ulStackSize plus guard");

	create_legacy_task(model, false);
	create_legacy_task(model, true);
	expect_true(model, model->stack_allocations == 1u,
		    "only explicit cacheable creation uses stack allocator");
	expect_true(model, model->ordinary_allocations == 2u,
		    "ordinary and static creation retain legacy allocation");

	reset_scenario(model);
	create_cacheable_task(model, false, true);
	expect_true(model, find_event(model, EVENT_READY_PUBLISH, 0u) == SIZE_MAX,
		    "release failure never publishes the task");
	expect_true(model, find_event(model, EVENT_ROLLBACK, 0u)
			    < find_event(model, EVENT_RETURN_FAILURE, 0u),
		    "successful rollback precedes recoverable creation failure");
	expect_true(model, !model->stack_allocated && !model->tcb_allocated,
		    "successful rollback frees stack and TCB completely");
	expect_true(model, !model->fail_stopped,
		    "successful rollback remains a recoverable creation failure");

	reset_scenario(model);
	create_cacheable_task(model, false, false);
	expect_true(model, model->fail_stopped,
		    "rollback exact-free failure enters fail-stop");
	expect_true(model, find_event(model, EVENT_READY_PUBLISH, 0u) == SIZE_MAX,
		    "rollback failure cannot publish the task");
	expect_true(model, find_event(model, EVENT_RETURN_FAILURE, 0u) == SIZE_MAX,
		    "rollback failure cannot return to the caller");

	reset_scenario(model);
	create_from_context(model, true, false);
	expect_true(model, find_event(model, EVENT_API_REJECT, 0u) != SIZE_MAX,
		    "ISR creation is rejected");
	expect_true(model, find_event(model, EVENT_STACK_ALLOC, 0u) == SIZE_MAX,
		    "ISR rejection occurs before allocation");

	reset_scenario(model);
	create_from_context(model, false, true);
	expect_true(model, find_event(model, EVENT_API_REJECT, 0u) != SIZE_MAX,
		    "IRQ-disabled task-context creation is rejected");
	expect_true(model, find_event(model, EVENT_KERNEL_LOCK, 0u) == SIZE_MAX,
		    "IRQ-disabled rejection occurs before kernel lock");
}

static void test_switch_and_first_restore(model_t *model)
{
	size_t begin = model->event_count;
	switch_task(model, 1u, true, true);
	size_t release = find_event(model, EVENT_RELEASE, begin);
	size_t select = find_event(model, EVENT_SELECT, begin);
	size_t acquire = find_event(model, EVENT_ACQUIRE_CORE1, begin);
	size_t restore = find_event(model, EVENT_RESTORE_CORE1, begin);

	expect_true(model, release < select, "switch-out release precedes selection");
	expect_true(model, select < acquire, "switch-in acquire follows selection");
	expect_true(model, acquire < restore, "switch-in acquire precedes restore");

	begin = model->event_count;
	first_task_restore(model, 0u, true);
	first_task_restore(model, 1u, true);
	size_t acquire0 = find_event(model, EVENT_ACQUIRE_CORE0, begin);
	size_t restore0 = find_event(model, EVENT_RESTORE_CORE0, begin);
	size_t acquire1 = find_event(model, EVENT_ACQUIRE_CORE1, begin);
	size_t restore1 = find_event(model, EVENT_RESTORE_CORE1, begin);

	expect_true(model, acquire0 < restore0, "AP core 0 first task acquires before restore");
	expect_true(model, acquire1 < restore1, "AP core 1 first task acquires before restore");

	reset_scenario(model);
	switch_task(model, 0u, false, true);
	expect_true(model, model->fail_stopped, "switch-out release failure enters fail-stop");
	expect_true(model, find_event(model, EVENT_SELECT, 0u) == SIZE_MAX,
		    "switch-out release failure prevents task selection");
	expect_true(model, find_event(model, EVENT_RESTORE_CORE0, 0u) == SIZE_MAX,
		    "switch-out release failure prevents restore");

	reset_scenario(model);
	switch_task(model, 1u, true, false);
	expect_true(model, model->fail_stopped, "switch-in acquire failure enters fail-stop");
	expect_true(model, find_event(model, EVENT_SELECT, 0u) != SIZE_MAX,
		    "switch-in acquire failure occurs after selection");
	expect_true(model, find_event(model, EVENT_RESTORE_CORE1, 0u) == SIZE_MAX,
		    "switch-in acquire failure prevents restore");

	reset_scenario(model);
	first_task_restore(model, 0u, false);
	expect_true(model, model->fail_stopped, "first-task acquire failure enters fail-stop");
	expect_true(model, find_event(model, EVENT_RESTORE_CORE0, 0u) == SIZE_MAX,
		    "first-task acquire failure prevents first restore");
}

static void test_delete_and_lock_order(model_t *model)
{
	size_t begin = model->event_count;
	model->px_stack = 0x60700000u;
	model->ul_stack_size = 0x1001u;
	model->stack_allocated = true;
	model->tcb_allocated = true;
	delete_task(model, true);
	size_t remove = find_event(model, EVENT_REMOVE_SCHEDULER, begin);
	size_t clear0 = find_event(model, EVENT_CLEAR_CURRENT_CORE0, begin);
	size_t clear1 = find_event(model, EVENT_CLEAR_CURRENT_CORE1, begin);
	size_t kernel_unlock_event = find_event(model, EVENT_KERNEL_UNLOCK, begin);
	size_t heap_lock_event = find_event(model, EVENT_HEAP_LOCK, begin);
	size_t acquire = find_event(model, EVENT_ACQUIRE_FULL_OWNER, begin);
	size_t guard = find_event(model, EVENT_GUARD_VALIDATE, begin);
	size_t exact_free = find_event(model, EVENT_EXACT_FREE, begin);
	size_t tcb_free = find_event(model, EVENT_TCB_FREE, begin);

	expect_true(model, remove < clear0 && clear0 < clear1,
		    "delete removes scheduler and current ownership first");
	expect_true(model, clear1 < kernel_unlock_event && kernel_unlock_event < heap_lock_event,
		    "delete ends kernel ownership before heap lock");
	expect_true(model, heap_lock_event < exact_free && exact_free < tcb_free,
		    "delete exact-frees stack before ordinary TCB free");
	expect_true(model, heap_lock_event < acquire && acquire < guard
			    && guard < exact_free,
		    "exact free acquires full owner span before guard validation");

	kernel_lock(model);
	expect_true(model, !heap_lock(model), "kernel-lock to heap-lock inversion is rejected");
	kernel_unlock(model);

	reset_scenario(model);
	model->px_stack = 0x60700000u;
	model->ul_stack_size = 0x1001u;
	model->stack_allocated = true;
	model->tcb_allocated = true;
	delete_task(model, false);
	expect_true(model, model->fail_stopped,
		    "delete exact-free or guard failure enters fail-stop");
	expect_true(model, find_event(model, EVENT_TCB_FREE, 0u) == SIZE_MAX,
		    "delete failure never frees TCB or returns");
}

static void test_direct_wdt_fail_stop(model_t *model)
{
	reset_scenario(model);
	fail_stop(model);
	size_t reason = find_event(model, EVENT_RECORD_FATAL_REASON, 0u);
	size_t core = find_event(model, EVENT_RECORD_FATAL_CORE, 0u);
	size_t dmb = find_event(model, EVENT_FATAL_DMB, 0u);
	size_t hook = find_event(model, EVENT_PLATFORM_RESET_HOOK, 0u);
	size_t set_reason = find_event(model, EVENT_SET_CRASH_RESET_REASON, 0u);
	size_t wdt = find_event(model, EVENT_WDT_FORCE_RESET, 0u);

	expect_true(model, reason < core && core < dmb && dmb < hook
			    && hook < set_reason && set_reason < wdt,
		    "fatal state and DMB precede direct reset reason and WDT");
	expect_true(model, find_event(model, EVENT_LOCAL_IRQ_DISABLE, 0u) == SIZE_MAX,
		    "non-returning WDT reset does not enter local fallback");
	expect_true(model, find_event(model, EVENT_SELECT, 0u) == SIZE_MAX,
		    "direct WDT fail-stop performs no later selection");
	expect_true(model, find_event(model, EVENT_RESTORE_CORE0, 0u) == SIZE_MAX
			    && find_event(model, EVENT_RESTORE_CORE1, 0u) == SIZE_MAX,
		    "direct WDT fail-stop performs no later restore");

	reset_scenario(model);
	fail_stop_with_wdt_return(model, true);
	wdt = find_event(model, EVENT_WDT_FORCE_RESET, 0u);
	size_t irq_disable = find_event(model, EVENT_LOCAL_IRQ_DISABLE, 0u);
	size_t loop = find_event(model, EVENT_FALLBACK_LOOP, 0u);

	expect_true(model, wdt < irq_disable && irq_disable < loop,
		    "local IRQ-off loop is only a returned-WDT fallback");
	expect_true(model, find_event(model, EVENT_SELECT, 0u) == SIZE_MAX,
		    "returned-reset fallback performs no selection");
	expect_true(model, find_event(model, EVENT_RESTORE_CORE0, 0u) == SIZE_MAX
			    && find_event(model, EVENT_RESTORE_CORE1, 0u) == SIZE_MAX,
		    "returned-reset fallback performs no restore");
	expect_true(model, model->select_after_fatal == 0u
			    && model->restore_after_fatal == 0u,
		    "all fatal scenarios have zero post-fatal select and restore");
}

int main(void)
{
	model_t model;

	memset(&model, 0, sizeof(model));
	test_creation_and_routes(&model);
	test_switch_and_first_restore(&model);
	test_delete_and_lock_order(&model);
	test_direct_wdt_fail_stop(&model);

	if (model.failures != 0u) {
		printf("RTOS-04:MODEL:FAIL checks=%u failures=%u\n",
		       model.checks, model.failures);
		return 1;
	}

	printf("RTOS-04:MODEL:PASS checks=%u failures=0 wdt_reset_calls=%u "
	       "select_after_fatal=%u restore_after_fatal=%u\n",
	       model.checks, model.wdt_reset_calls, model.select_after_fatal,
	       model.restore_after_fatal);
	return 0;
}
