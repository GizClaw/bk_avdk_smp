#ifndef CACHE_CONTRACT_TEST_ARMSTAR_H
#define CACHE_CONTRACT_TEST_ARMSTAR_H

#include <stdint.h>

#define SCB_CLIDR_DC_Msk UINT32_C(1)
#define __SCB_DCACHE_LINE_SIZE UINT32_C(32)

typedef struct {
	uint32_t CLIDR;
} SCB_Type;

extern SCB_Type g_cache_test_scb;

#define SCB (&g_cache_test_scb)
#define __DSB() cache_test_dsb()
#define __ISB() cache_test_isb()

void SCB_CleanDCache_by_Addr(void *addr, int32_t size);
void SCB_InvalidateDCache_by_Addr(void *addr, int32_t size);
void SCB_CleanInvalidateDCache_by_Addr(void *addr, int32_t size);
void SCB_CleanInvalidateDCache(void);
void SCB_DisableDCache(void);
void SCB_EnableDCache(void);
void SCB_InvalidateICache(void);
void cache_test_dsb(void);
void cache_test_isb(void);

#endif
