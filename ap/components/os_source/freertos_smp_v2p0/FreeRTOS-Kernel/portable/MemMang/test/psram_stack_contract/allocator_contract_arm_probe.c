#if defined(PSRAM_STACK_ALLOCATOR_ARM_PROBE)

#include "psram_stack_allocator.h"

int allocator_contract_arm_probe(size_t requested_size)
{
	void *payload = NULL;
	int result = psram_stack_alloc(requested_size, &payload);

	result |= psram_stack_cache_release(payload, requested_size);
	result |= psram_stack_cache_acquire(payload, requested_size);
	result |= psram_stack_free_exact(payload, requested_size);
	return result;
}

#else

int allocator_contract_arm_probe_disabled(void)
{
	return 0;
}

#endif
