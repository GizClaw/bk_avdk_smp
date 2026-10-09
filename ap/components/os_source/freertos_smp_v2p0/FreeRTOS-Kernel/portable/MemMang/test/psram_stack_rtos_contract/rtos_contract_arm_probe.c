#include <stddef.h>
#include <stdint.h>

#include "psram_stack_allocator.h"

typedef struct {
	void *px_stack;
	size_t ul_stack_size;
} rtos_owner_tcb_contract_t;

_Static_assert(sizeof(((rtos_owner_tcb_contract_t *)0)->px_stack) == sizeof(void *),
	       "existing stack address must preserve pointer width");
_Static_assert(sizeof(((rtos_owner_tcb_contract_t *)0)->ul_stack_size) == sizeof(size_t),
	       "existing stack size must preserve target size width");

int rtos_owner_arm_contract_probe(size_t requested_size)
{
	rtos_owner_tcb_contract_t tcb = { 0 };
	int result;

	tcb.ul_stack_size = requested_size;
	result = psram_stack_alloc(requested_size, &tcb.px_stack);
	if (result != 0) {
		return result;
	}

	result = psram_stack_cache_release(tcb.px_stack, tcb.ul_stack_size);
	if (result != 0) {
		return result;
	}

	result = psram_stack_cache_acquire(tcb.px_stack, tcb.ul_stack_size);
	if (result != 0) {
		return result;
	}

	return psram_stack_free_exact(tcb.px_stack, tcb.ul_stack_size);
}
