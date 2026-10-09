#ifndef RTOS04_TEST_ARMSTAR_H
#define RTOS04_TEST_ARMSTAR_H

#include <stdint.h>

typedef struct {
	uint32_t RBAR;
	uint32_t RLAR;
} ARM_MPU_Region_t;

#define ARM_MPU_SH_NON 0u
#define ARM_MPU_SH_INNER 1u

#define ARM_MPU_RBAR(base, sh, ro, np, xn) \
	(((uint32_t)(base) & 0xffffffe0u) | (((uint32_t)(sh) & 0x3u) << 3) | \
	 (((uint32_t)(ro) & 0x1u) << 2) | (((uint32_t)(np) & 0x1u) << 1) | \
	 ((uint32_t)(xn) & 0x1u))
#define ARM_MPU_RLAR(limit, index) \
	(((uint32_t)(limit) & 0xffffffe0u) | \
	 (((uint32_t)(index) & 0x7u) << 1) | 0x1u)
#define ARM_MPU_ATTR(outer, inner) \
	((uint8_t)((((uint32_t)(outer) & 0xfu) << 4) | ((uint32_t)(inner) & 0xfu)))

#endif
