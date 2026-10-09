#if defined(CACHE_CONTRACT_ARM_PROBE)

#include <stddef.h>

#include "cache_psram_stack_internal.h"

int cache_contract_arm_probe(void *addr, size_t size)
{
	int result = (int)psram_stack_heap_overlap(addr, size);

	result |= cache_stack_release_aligned(addr, size);
	result |= cache_stack_acquire_aligned(addr, size);
	result |= cache_stack_metadata_clean_aligned(addr, size);
	return result;
}

#else

int cache_contract_arm_probe_disabled(void)
{
	return 0;
}

#endif
