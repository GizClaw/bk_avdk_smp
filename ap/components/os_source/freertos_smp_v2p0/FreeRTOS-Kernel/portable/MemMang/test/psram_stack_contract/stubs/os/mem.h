#ifndef PSRAM_STACK_TEST_OS_MEM_H
#define PSRAM_STACK_TEST_OS_MEM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define os_memset memset
#define os_memcpy memcpy

#if CONFIG_MEM_DEBUG
void *psram_malloc_cm(
	const char *call_func_name,
	int line,
	size_t size,
	int need_zero);
#define psram_malloc(size) \
	psram_malloc_cm((const char *)__FUNCTION__, __LINE__, (size), 0)
uint32_t bk_psram_heap_get_used_count(void);
void bk_psram_heap_get_used_state(void);
#endif

static inline void os_memset_word(uint32_t *destination, uint32_t value, size_t size)
{
	size_t words = size / sizeof(*destination);

	for (size_t index = 0U; index < words; index++) {
		destination[index] = value;
	}
}

#endif
