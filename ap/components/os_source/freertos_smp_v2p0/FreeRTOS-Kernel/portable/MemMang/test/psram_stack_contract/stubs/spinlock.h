#ifndef PSRAM_STACK_TEST_SPINLOCK_H
#define PSRAM_STACK_TEST_SPINLOCK_H

#if defined(PSRAM_STACK_HOST_TEST)
#include <pthread.h>
typedef struct {
	pthread_mutex_t mutex;
} spinlock_t;
#define SPIN_LOCK_ACQUIRE_INIT {PTHREAD_MUTEX_INITIALIZER}
#else
typedef struct {
	unsigned int value;
} spinlock_t;
#define SPIN_LOCK_ACQUIRE_INIT {0U}
#endif

#define SPINLOCK_SECTION
void vPortEnterCritical(spinlock_t *lock);
void vPortExitCritical(spinlock_t *lock);

#endif
