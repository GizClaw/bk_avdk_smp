#ifndef PM05C_STUB_OS_OS_H
#define PM05C_STUB_OS_OS_H

#include <stdbool.h>
#include <stdint.h>
#include <common/bk_err.h>

typedef void *beken_mutex_t;
typedef void *SemaphoreHandle_t;
typedef struct {
	uint64_t storage[16];
} StaticSemaphore_t;

#define BEKEN_WAIT_FOREVER UINT32_MAX
#define portMAX_DELAY UINT32_MAX
#define pdPASS 1

bk_err_t rtos_init_mutex(beken_mutex_t *mutex);
bk_err_t rtos_lock_mutex(beken_mutex_t *mutex);
bk_err_t rtos_unlock_mutex(beken_mutex_t *mutex);
bool rtos_is_in_interrupt_context(void);
bool rtos_local_irq_disabled(void);
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);
int xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t timeout);
int xSemaphoreGive(SemaphoreHandle_t mutex);

#endif
