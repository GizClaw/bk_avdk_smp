#ifndef PSRAM_STACK_TEST_FREERTOS_H
#define PSRAM_STACK_TEST_FREERTOS_H

#include "common/bk_include.h"

typedef struct {
	size_t xAvailableHeapSpaceInBytes;
	size_t xSizeOfLargestFreeBlockInBytes;
	size_t xSizeOfSmallestFreeBlockInBytes;
	size_t xNumberOfFreeBlocks;
	size_t xMinimumEverFreeBytesRemaining;
	size_t xNumberOfSuccessfulAllocations;
	size_t xNumberOfSuccessfulFrees;
} HeapStats_t;

#if CONFIG_MEM_DEBUG
void *pvPortMalloc_cm(
	const char *call_func_name,
	int line,
	size_t size,
	int need_zero);
void *vPortFree_cm(const char *call_func_name, int line, void *pointer);
#define pvPortMalloc(size) \
	pvPortMalloc_cm((const char *)__FUNCTION__, __LINE__, (size), 1)
#define vPortFree(pointer) \
	vPortFree_cm((const char *)__FUNCTION__, __LINE__, (pointer))
#endif

#endif
