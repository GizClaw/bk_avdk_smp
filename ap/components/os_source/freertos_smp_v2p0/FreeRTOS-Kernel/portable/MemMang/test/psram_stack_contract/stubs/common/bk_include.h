#ifndef PSRAM_STACK_TEST_BK_INCLUDE_H
#define PSRAM_STACK_TEST_BK_INCLUDE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint8_t u8;
typedef void *TaskHandle_t;

extern _Alignas(32) uint8_t g_psram_stack_test_heap[4096];
extern _Alignas(32) uint8_t g_psram_test_ordinary_heap[4096];
extern uint32_t g_fixed_psram_power_down;
extern uint32_t g_fixed_psram_used_count;

void cachealloc_test_assert(int condition, const char *expression);
void pm05a_test_fatal_reset(void);
void pm05a_test_dmb(void);

#ifndef CONFIG_CACHEABLE_PSRAM_TASK_STACK
#define CONFIG_CACHEABLE_PSRAM_TASK_STACK 1
#endif
#define CONFIG_PSRAM_AS_SYS_MEMORY 1
#define CONFIG_FREERTOS_SMP 1
#define CONFIG_HEAP_IN_CACHE 0
#ifndef CONFIG_MEM_DEBUG
#define CONFIG_MEM_DEBUG 1
#endif
#if CONFIG_MEM_DEBUG != 1
#error "CACHEALLOC-02 requires CONFIG_MEM_DEBUG=1"
#endif
#define CONFIG_MEM_DEBUG_OVERFLOW 0
#define CONFIG_MEM_DEBUG_FUNC_NAME 0
#define CONFIG_MEM_DEBUG_TASK_NAME 0
#define CONFIG_MALLOC_STATIS 0
#define CONFIG_MEMLEAK_DETECT 0
#define CONFIG_DEBUG_VERSION 0
#define CONFIG_PSRAM_HEAP_INIT_SET_ZERO 0
#define CONFIG_FIX_PSRAM_PTR_ISSUE 1
#define CONFIG_TZ 0
#define CONFIG_SPE 0

#define CONFIG_AP_PSRAM_STACK_HEAP_ADDR \
	((uintptr_t)g_psram_stack_test_heap)
#define CONFIG_AP_PSRAM_STACK_HEAP_SIZE ((size_t)4096U)
#define CONFIG_AP_PSRAM_HEAP_ADDR ((uintptr_t)g_psram_test_ordinary_heap)
#define CONFIG_AP_PSRAM_HEAP_SIZE ((size_t)4096U)

#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configDYNAMIC_HEAP_SIZE 0
#define configAPPLICATION_ALLOCATED_HEAP 0
#define configTOTAL_HEAP_SIZE 4096U
#define configTICK_RATE_HZ 1000U

#define portBYTE_ALIGNMENT 8U
#define portBYTE_ALIGNMENT_MASK (portBYTE_ALIGNMENT - 1U)

#define BK_LOG_INFO 0
#define BK_LOG_DEBUG 0
#define BK_LOGD(...) ((void)0)
#define BK_DUMP_OUT(...) ((void)0)
#define bk_printf_static_nonblock(...) ((void)0)
#define BK_ASSERT(condition) cachealloc_test_assert((condition), #condition)
#define configASSERT(condition) cachealloc_test_assert((condition), #condition)
#define configPSRAM_STACK_FATAL_RESET() pm05a_test_fatal_reset()
#define mtCOVERAGE_TEST_MARKER() ((void)0)
#define traceMALLOC(pointer, size) ((void)(pointer), (void)(size))
#define traceFREE(pointer, size) ((void)(pointer), (void)(size))
#define __DMB() pm05a_test_dmb()

#define SOC_ADDR_OFFSET 0U
#define SOC_PSRAM_DATA_BASE UINT32_C(0x60000000)
#define SOC_PSRAM_DATA_SIZE UINT32_C(0x01000000)
#define PM_PSRAM_POWER_DOWN_MAGIC UINT32_C(0x5053524d)
#define PM_POWER_PSRAM_MODULE_NAME_AS_MEM 0
#define PM_POWER_MODULE_STATE_ON 1
#define FIXED_ADDR_PSRAM_POWER_DOWN g_fixed_psram_power_down
#define FIXED_ADDR_PSRAM_USDE_COUNT g_fixed_psram_used_count

#endif
