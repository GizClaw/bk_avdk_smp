#ifndef PSRAM_STACK_TEST_TASK_H
#define PSRAM_STACK_TEST_TASK_H

#include "FreeRTOS.h"

UBaseType_t xTaskGetTickCount(void);
const char *pcTaskGetName(TaskHandle_t task);
int rtos_is_scheduler_started(void);

#endif
