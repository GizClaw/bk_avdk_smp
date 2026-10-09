// Copyright 2023 Espressif Systems (Shanghai) PTE LTD
// Copyright 2024 Beken Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
/*
* Change Logs:
* Date			 Author 	  Notes
* 2024-05-05	 Beken	  adapter to Beken sdk
*/

#include "sdkconfig.h"
#include "freertos_additions.h"
#include <os/mem.h>

#if defined(CONFIG_CACHEABLE_PSRAM_TASK_STACK) && CONFIG_CACHEABLE_PSRAM_TASK_STACK
#include "../../portable/MemMang/psram_stack_allocator.h"
#endif

/*
 * Both StaticTask_t and TCB_t structures are provided by FreeRTOS sources.
 * This is just an additional check of the consistency of these structures.
 */
_Static_assert( offsetof( StaticTask_t, pxDummy6 ) == offsetof( TCB_t, pxStack ) );
_Static_assert( offsetof( StaticTask_t, pxDummy8 ) == offsetof( TCB_t, pxEndOfStack ) );
_Static_assert( sizeof( TCB_t ) == sizeof( StaticTask_t ) );
_Static_assert( _Alignof( TCB_t ) == _Alignof( StaticTask_t ) );

/* ------------------------------------------------- Kernel Control ------------------------------------------------- */

/*
 * Wrapper function to take "xKerneLock"
 */
    void prvTakeKernelLock( void )
    {
        /* We call the tasks.c critical section macro to take xKernelLock */
        taskENTER_CRITICAL( &xKernelLock );
    }

/*----------------------------------------------------------*/

/*
 * Wrapper function to release "xKerneLock"
 */
    void prvReleaseKernelLock( void )
    {
        /* We call the tasks.c critical section macro to release xKernelLock */
        taskEXIT_CRITICAL( &xKernelLock );
    }

/*----------------------------------------------------------*/

/*----------------------------------------------------------*/

#if (1)

    BaseType_t xTaskIncrementTickOtherCores( void )
    {
        /* Minor optimization. This function can never switch cores mid
         * execution */
        BaseType_t xCoreID = portGET_CORE_ID();
        BaseType_t xSwitchRequired = pdFALSE;

        /* This function should never be called by Core 0. */
        configASSERT( xCoreID != 0 );

        /* Called by the portable layer each time a tick interrupt occurs.
         * Increments the tick then checks to see if the new tick value will
         * cause any tasks to be unblocked. */
        traceTASK_INCREMENT_TICK( xTickCount );

        if( uxSchedulerSuspended[ xCoreID ] == ( UBaseType_t ) 0U )
        {
            /* We need take the kernel lock here as we are about to access
             * kernel data structures. */
            taskENTER_CRITICAL_ISR( &xKernelLock );

            /* A task being unblocked cannot cause an immediate context switch
             * if preemption is turned off. */
            #if ( configUSE_PREEMPTION == 1 )
            {
                /* Check if core 0 calling xTaskIncrementTick() has
                 * unblocked a task that can be run. */
                if( uxTopReadyPriority > pxCurrentTCBs[ xCoreID ]->uxPriority )
                {
                    xSwitchRequired = pdTRUE;
                }
                else
                {
                    mtCOVERAGE_TEST_MARKER();
                }
            }
            #endif /* if ( configUSE_PREEMPTION == 1 ) */

            /* Tasks of equal priority to the currently running task will share
             * processing time (time slice) if preemption is on, and the application
             * writer has not explicitly turned time slicing off. */
            #if ( ( configUSE_PREEMPTION == 1 ) && ( configUSE_TIME_SLICING == 1 ) )
            {
                if( listCURRENT_LIST_LENGTH( &( pxReadyTasksLists[ pxCurrentTCBs[ xCoreID ]->uxPriority ] ) ) > ( UBaseType_t ) 1 )
                {
                    xSwitchRequired = pdTRUE;
                }
                else
                {
                    mtCOVERAGE_TEST_MARKER();
                }
            }
            #endif /* ( ( configUSE_PREEMPTION == 1 ) && ( configUSE_TIME_SLICING == 1 ) ) */

            /* Release the previously taken kernel lock as we have finished
             * accessing the kernel data structures. */
            taskEXIT_CRITICAL_ISR( &xKernelLock );

            #if ( configUSE_PREEMPTION == 1 )
            {
                if( xYieldPending[ xCoreID ] != pdFALSE )
                {
                    xSwitchRequired = pdTRUE;
                }
                else
                {
                    mtCOVERAGE_TEST_MARKER();
                }
            }
            #endif /* configUSE_PREEMPTION */
        }

        #if ( configUSE_TICK_HOOK == 1 )
        {
            vApplicationTickHook();
        }
        #endif

        return xSwitchRequired;
    }

#endif /* (1 && ( configNUM_CORES > 1 ) ) */
/*----------------------------------------------------------*/

/* -------------------------------------------------- Task Creation ------------------------------------------------- */

#if ( configSUPPORT_DYNAMIC_ALLOCATION == 1 )

    #if defined(CONFIG_CACHEABLE_PSRAM_TASK_STACK) && CONFIG_CACHEABLE_PSRAM_TASK_STACK

        static void prvFreeUnpublishedCacheablePsramTask(
            TCB_t * const pxTCB,
            void * const pvStack,
            const size_t xStackSize,
            const BaseType_t xTcbInitialized )
        {
            psram_stack_pm_transition_t xTransition = {
                PSRAM_STACK_PM_TRANSITION_NONE, 0U
            };

            if( ( pxTCB != NULL ) && ( xTcbInitialized != pdFALSE ) )
            {
                #if ( ( configUSE_NEWLIB_REENTRANT == 1 ) || ( configUSE_C_RUNTIME_TLS_SUPPORT == 1 ) )
                {
                    configDEINIT_TLS_BLOCK( pxTCB->xTLSBlock );
                }
                #endif
            }
            const int xFreeStatus = psram_stack_free_exact_with_transition(
                pvStack, xStackSize, &xTransition );

            if( xFreeStatus != PSRAM_STACK_STATUS_OK )
            {
                prvPsramStackKernelFatal(
                    psramSTACK_FATAL_CREATE_ROLLBACK );
            }
            if( xTransition.kind == PSRAM_STACK_PM_TRANSITION_LAST_FREE )
            {
                if( configPSRAM_STACK_PM_VOTE_RELEASED() == BK_OK )
                {
                    if( psram_stack_pm_hold_release_commit(
                            xTransition.generation )
                        != PSRAM_STACK_STATUS_OK )
                    {
                        prvPsramStackKernelFatal(
                            psramSTACK_FATAL_CREATE_ROLLBACK );
                    }
                }
                else if( psram_stack_pm_hold_release_abort(
                             xTransition.generation )
                         != PSRAM_STACK_STATUS_OK )
                {
                    prvPsramStackKernelFatal(
                        psramSTACK_FATAL_CREATE_ROLLBACK );
                }
            }
            if( pxTCB != NULL )
            {
                vPortFree( pxTCB );
            }
        }

        static BaseType_t prvAllocateCacheablePsramTask(
            const uint32_t ulStackDepth,
            TCB_t ** const ppxNewTCB,
            psram_stack_pm_transition_t * const pxTransition )
        {
            void * pvStack;
            TCB_t * pxNewTCB;
            const size_t xStackSize = ( ( size_t ) ulStackDepth ) * sizeof( StackType_t );

            if( psram_stack_alloc_with_transition(
                    xStackSize, &pvStack, pxTransition )
                != PSRAM_STACK_STATUS_OK )
            {
                return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
            }

            pxNewTCB = ( TCB_t * ) psram_malloc( sizeof( TCB_t ) );
            if( pxNewTCB == NULL )
            {
                prvFreeUnpublishedCacheablePsramTask(
                    NULL, pvStack, xStackSize, pdFALSE );
                return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
            }

            memset( ( void * ) pxNewTCB, 0x00, sizeof( TCB_t ) );
            pxNewTCB->pxStack = ( StackType_t * ) pvStack;

            #if ( tskSTATIC_AND_DYNAMIC_ALLOCATION_POSSIBLE != 0 )
            {
                pxNewTCB->ucStaticallyAllocated = tskDYNAMICALLY_ALLOCATED_STACK_AND_TCB;
            }
            #endif

            *ppxNewTCB = pxNewTCB;
            return pdPASS;
        }

        static BaseType_t prvResyncPendingCacheablePsramHold( void )
        {
            uint64_t ullPendingGeneration = 0U;

            if( psram_stack_pm_hold_release_claim( &ullPendingGeneration )
                != PSRAM_STACK_STATUS_OK )
            {
                return pdFAIL;
            }
            if( configPSRAM_STACK_PM_VOTE_RELEASED() != BK_OK )
            {
                if( psram_stack_pm_hold_release_abort( ullPendingGeneration )
                    != PSRAM_STACK_STATUS_OK )
                {
                    prvPsramStackKernelFatal(
                        psramSTACK_FATAL_CREATE_ROLLBACK );
                }
                return pdFAIL;
            }
            if( psram_stack_pm_hold_release_commit( ullPendingGeneration )
                != PSRAM_STACK_STATUS_OK )
            {
                prvPsramStackKernelFatal(
                    psramSTACK_FATAL_CREATE_ROLLBACK );
            }
            return pdPASS;
        }

    #endif

 
    BaseType_t xTaskCreate_ex( TaskFunction_t pxTaskCode,
                                const char * const pcName,
                                const uint32_t usStackDepth,
                                void * const pvParameters,
                                UBaseType_t uxPriority,
                                TaskHandle_t * const pxCreatedTask,
                                const BaseType_t xCoreID,
                                beken_mem_type_t eMemType)
    {
        BaseType_t xReturn;

        configASSERT( taskVALID_CORE_ID( xCoreID ) == pdTRUE || xCoreID == tskNO_AFFINITY );

        {
            TCB_t * pxNewTCB;

            /* If the stack grows down then allocate the stack then the TCB so the
             * stack does not grow into the TCB.  Likewise if the stack grows up
             * then allocate the TCB then the stack. */
            #if ( portSTACK_GROWTH > 0 )
            {
                switch (eMemType)
                {
                    case HEAP_MEM_TYPE_PSRAM:
    #if CONFIG_PSRAM_AS_SYS_MEMORY
                    {
                        /* Allocate space for the TCB.  Where the memory comes from depends on
                        * the implementation of the port malloc function and whether or not static
                        * allocation is being used. */

                        pxNewTCB = ( TCB_t * ) psram_malloc( sizeof( TCB_t ) );

                        if( pxNewTCB != NULL )
                        {
                            memset( ( void * ) pxNewTCB, 0x00, sizeof( TCB_t ) );
                            /* Allocate space for the stack used by the task being created.
                            * The base of the stack memory stored in the TCB so the task can
                            * be deleted later if required. */
                            pxNewTCB->pxStack = ( StackType_t * ) psram_malloc( ( ( ( size_t ) usStackDepth ) * sizeof( StackType_t ) ) ); /*lint !e961 MISRA exception as the casts are only redundant for some ports. */

                            if( pxNewTCB->pxStack == NULL )
                            {
                                /* Could not allocate the stack.  Delete the allocated TCB. */
                                vPortFree( pxNewTCB );
                                pxNewTCB = NULL;
                            }
                        }
                    }
    #else //#if CONFIG_PSRAM_AS_SYS_MEMORY
                        return pdFAIL;
    #endif
                        break;
                    case HEAP_MEM_TYPE_DEFAULT:
                    default:
                    {
                        /* Allocate space for the TCB.  Where the memory comes from depends on
                        * the implementation of the port malloc function and whether or not static
                        * allocation is being used. */
                        pxNewTCB = ( TCB_t * ) pvPortMalloc( sizeof( TCB_t ) );

                        if( pxNewTCB != NULL )
                        {
                            memset( ( void * ) pxNewTCB, 0x00, sizeof( TCB_t ) );

                            /* Allocate space for the stack used by the task being created.
                            * The base of the stack memory stored in the TCB so the task can
                            * be deleted later if required. */
                            pxNewTCB->pxStack = ( StackType_t * ) pvPortMalloc( ( ( ( size_t ) usStackDepth ) * sizeof( StackType_t ) ) ); /*lint !e961 MISRA exception as the casts are only redundant for some ports. */

                            if( pxNewTCB->pxStack == NULL )
                            {
                                /* Could not allocate the stack.  Delete the allocated TCB. */
                                vPortFree( pxNewTCB );
                                pxNewTCB = NULL;
                            }
                        }
                    }
                    break;
                }
            }

            #else /* portSTACK_GROWTH */
            {
                switch (eMemType)
                {
                    case HEAP_MEM_TYPE_PSRAM:
#if CONFIG_PSRAM_AS_SYS_MEMORY
                    {
                        StackType_t * pxStack;

                        /* Allocate space for the stack used by the task being created. */
                        pxStack = psram_malloc( ( ( ( size_t ) usStackDepth ) * sizeof( StackType_t ) ) ); /*lint !e9079 All values returned by pvPortMalloc() have at least the alignment required by the MCU's stack and this allocation is the stack. */

                        if( pxStack != NULL )
                        {
                            /* Allocate space for the TCB. */
                            pxNewTCB = ( TCB_t * ) psram_malloc( sizeof( TCB_t ) ); /*lint !e9087 !e9079 All values returned by pvPortMalloc() have at least the alignment required by the MCU's stack, and the first member of TCB_t is always a pointer to the task's stack. */

                            if( pxNewTCB != NULL )
                            {
                                memset( ( void * ) pxNewTCB, 0x00, sizeof( TCB_t ) );

                                /* Store the stack location in the TCB. */
                                pxNewTCB->pxStack = pxStack;
                            }
                            else
                            {
                                /* The stack cannot be used as the TCB was not created.  Free
                                * it again. */
                                vPortFree( pxStack );
                            }
                        }
                        else
                        {
                            pxNewTCB = NULL;
                        }
                    }
#else //#if CONFIG_PSRAM_AS_SYS_MEMORY
                    return pdFAIL;
#endif //#if CONFIG_PSRAM_AS_SYS_MEMORY
                    break;

                    case HEAP_MEM_TYPE_DEFAULT:
                    default:
                    {

                        StackType_t * pxStack;

                        /* Allocate space for the stack used by the task being created. */
                        pxStack = pvPortMalloc( ( ( ( size_t ) usStackDepth ) * sizeof( StackType_t ) ) ); /*lint !e9079 All values returned by pvPortMalloc() have at least the alignment required by the MCU's stack and this allocation is the stack. */

                        if( pxStack != NULL )
                        {
                            /* Allocate space for the TCB. */
                            pxNewTCB = ( TCB_t * ) pvPortMalloc( sizeof( TCB_t ) ); /*lint !e9087 !e9079 All values returned by pvPortMalloc() have at least the alignment required by the MCU's stack, and the first member of TCB_t is always a pointer to the task's stack. */

                            if( pxNewTCB != NULL )
                            {
                                memset( ( void * ) pxNewTCB, 0x00, sizeof( TCB_t ) );

                                /* Store the stack location in the TCB. */
                                pxNewTCB->pxStack = pxStack;
                            }
                            else
                            {
                                /* The stack cannot be used as the TCB was not created.  Free
                                * it again. */
                                vPortFree( pxStack );
                            }
                        }
                        else
                        {
                            pxNewTCB = NULL;
                        }
                    }
                    break;
                }
            }
            #endif /* portSTACK_GROWTH */

            if( pxNewTCB != NULL )
            {
                #if ( tskSTATIC_AND_DYNAMIC_ALLOCATION_POSSIBLE != 0 ) /*lint !e9029 !e731 Macro has been consolidated for readability reasons. */
                {
                    /* Tasks can be created statically or dynamically, so note this
                     * task was created dynamically in case it is later deleted. */
                    pxNewTCB->ucStaticallyAllocated = tskDYNAMICALLY_ALLOCATED_STACK_AND_TCB;
                }
                #endif /* tskSTATIC_AND_DYNAMIC_ALLOCATION_POSSIBLE */

                prvInitialiseNewTask( pxTaskCode, pcName, ( uint32_t ) usStackDepth, pvParameters, uxPriority, pxCreatedTask, pxNewTCB, NULL, xCoreID );
                prvAddNewTaskToReadyList( pxNewTCB );
                xReturn = pdPASS;
            }
            else
            {
                xReturn = errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
            }
        }

        return xReturn;
    }


    BaseType_t xTaskCreatePinnedToCore( TaskFunction_t pxTaskCode,
                                        const char * const pcName,
                                        const uint32_t usStackDepth,
                                        void * const pvParameters,
                                        UBaseType_t uxPriority,
                                        TaskHandle_t * const pxCreatedTask,
                                        const BaseType_t xCoreID ) 
    {
        return xTaskCreate_ex(pxTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pxCreatedTask, xCoreID, HEAP_MEM_TYPE_DEFAULT);                               
    }

    BaseType_t xTaskCreateInPsram( TaskFunction_t pxTaskCode,
                            const char * const pcName, /*lint !e971 Unqualified char types are allowed for strings and single characters only. */
                            const configSTACK_DEPTH_TYPE usStackDepth,
                            void * const pvParameters,
                            UBaseType_t uxPriority,
                            TaskHandle_t * const pxCreatedTask,
                            const BaseType_t xCoreID)
    {
#if defined(CONFIG_CACHEABLE_PSRAM_TASK_STACK) && CONFIG_CACHEABLE_PSRAM_TASK_STACK
        BaseType_t xReturn = pdFAIL;
        BaseType_t xTcbInitialized = pdFALSE;
        BaseType_t xPM_IS_ACTIVE = pdFALSE;
        TCB_t * pxNewTCB = NULL;
        psram_stack_pm_transition_t xPmTransition = {
            PSRAM_STACK_PM_TRANSITION_NONE, 0U
        };

        configASSERT( taskVALID_CORE_ID( xCoreID ) == pdTRUE || xCoreID == tskNO_AFFINITY );
        if( ( xPortInIsrContext() != pdFALSE )
            || ( platform_local_irq_disabled() != false )
            || ( xSchedulerRunning == pdFALSE ) )
        {
            return pdFAIL;
        }
        if( configPSRAM_STACK_PM_ENSURE_READY() != BK_OK )
        {
            return pdFAIL;
        }
        xReturn = prvAllocateCacheablePsramTask(
            ( uint32_t ) usStackDepth, &pxNewTCB, &xPmTransition );
        if( ( xReturn != pdPASS )
            && ( prvResyncPendingCacheablePsramHold() == pdPASS ) )
        {
            xPmTransition.kind = PSRAM_STACK_PM_TRANSITION_NONE;
            xPmTransition.generation = 0U;
            xReturn = prvAllocateCacheablePsramTask(
                ( uint32_t ) usStackDepth, &pxNewTCB, &xPmTransition );
        }
        if( ( xReturn == pdPASS )
            && ( xPmTransition.kind != PSRAM_STACK_PM_TRANSITION_NONE )
            && ( xPmTransition.kind
                != PSRAM_STACK_PM_TRANSITION_FIRST_LIVE ) )
        {
            prvPsramStackKernelFatal(
                psramSTACK_FATAL_CREATE_ROLLBACK );
        }
        if( xReturn == pdPASS )
        {
            taskENTER_CRITICAL( &xKernelLock );
            {
                prvInitialiseNewTask( pxTaskCode, pcName,
                                      ( uint32_t ) usStackDepth,
                                      pvParameters, uxPriority, NULL,
                                      pxNewTCB, NULL, xCoreID );
                xTcbInitialized = pdTRUE;
                if( psram_stack_cache_release(
                        pxNewTCB->pxStack,
                        pxNewTCB->ulStackSize )
                    != PSRAM_STACK_STATUS_OK )
                {
                    xReturn = errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
                }
            }
            taskEXIT_CRITICAL( &xKernelLock );

            if( xReturn == pdPASS )
            {
                if( configPSRAM_STACK_PM_VOTE_ACTIVE() == BK_OK )
                {
                    xPM_IS_ACTIVE = pdTRUE;
                }
                else
                {
                    xReturn = errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
                }
            }
            if( xReturn != pdPASS )
            {
                prvFreeUnpublishedCacheablePsramTask(
                    pxNewTCB, pxNewTCB->pxStack,
                    pxNewTCB->ulStackSize, xTcbInitialized );
            }
            else
            {
                taskENTER_CRITICAL( &xKernelLock );
                if( xPM_IS_ACTIVE != pdFALSE )
                {
                    #define prvAddNewTaskToReadyList( task ) \
                        prvAddNewTaskToReadyListLocked( task )
                    prvAddNewTaskToReadyList( pxNewTCB );
                    #undef prvAddNewTaskToReadyList
                    if( pxCreatedTask != NULL )
                    {
                        *pxCreatedTask = ( TaskHandle_t ) pxNewTCB;
                    }
                }
                taskEXIT_CRITICAL( &xKernelLock );
            }
        }
        return xReturn;
#else
        return xTaskCreate_ex(pxTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pxCreatedTask, xCoreID, HEAP_MEM_TYPE_PSRAM);
#endif
    }

    #if defined(CONFIG_CACHEABLE_PSRAM_TASK_STACK) && CONFIG_CACHEABLE_PSRAM_TASK_STACK

        BaseType_t xTaskCreateInNoncachePsram( TaskFunction_t pxTaskCode,
                                               const char * const pcName,
                                               const configSTACK_DEPTH_TYPE usStackDepth,
                                               void * const pvParameters,
                                               UBaseType_t uxPriority,
                                               TaskHandle_t * const pxCreatedTask,
                                               const BaseType_t xCoreID )
        {
            return xTaskCreate_ex( pxTaskCode, pcName, usStackDepth, pvParameters,
                                   uxPriority, pxCreatedTask, xCoreID,
                                   HEAP_MEM_TYPE_PSRAM );
        }

    #endif

    

#endif /* ( configSUPPORT_DYNAMIC_ALLOCATION == 1 ) */
/*----------------------------------------------------------*/

#if ( configSUPPORT_STATIC_ALLOCATION == 1 )

    TaskHandle_t xTaskCreateStaticPinnedToCore( TaskFunction_t pxTaskCode,
                                                const char * const pcName,
                                                const uint32_t ulStackDepth,
                                                void * const pvParameters,
                                                UBaseType_t uxPriority,
                                                StackType_t * const puxStackBuffer,
                                                StaticTask_t * const pxTaskBuffer,
                                                const BaseType_t xCoreID )
    {
        TaskHandle_t xReturn;

        configASSERT( ( puxStackBuffer ) );
        configASSERT( ( pxTaskBuffer ) );
        configASSERT( taskVALID_CORE_ID( xCoreID ) == pdTRUE || xCoreID == tskNO_AFFINITY );

        {
            TCB_t * pxNewTCB;

            #if ( configASSERT_DEFINED == 1 )
            {
                /* Sanity check that the size of the structure used to declare a
                 * variable of type StaticTask_t equals the size of the real task
                 * structure. */
                volatile size_t xSize = sizeof( StaticTask_t );
                configASSERT( xSize == sizeof( TCB_t ) );
                ( void ) xSize; /* Prevent lint warning when configASSERT() is not used. */
            }
            #endif /* configASSERT_DEFINED */

            if( ( pxTaskBuffer != NULL ) && ( puxStackBuffer != NULL ) )
            {
                /* The memory used for the task's TCB and stack are passed into this
                 * function - use them. */
                pxNewTCB = ( TCB_t * ) pxTaskBuffer; /*lint !e740 !e9087 Unusual cast is ok as the structures are designed to have the same alignment, and the size is checked by an assert. */
                memset( ( void * ) pxNewTCB, 0x00, sizeof( TCB_t ) );
                pxNewTCB->pxStack = ( StackType_t * ) puxStackBuffer;

                #if ( tskSTATIC_AND_DYNAMIC_ALLOCATION_POSSIBLE != 0 ) /*lint !e731 !e9029 Macro has been consolidated for readability reasons. */
                {
                    /* Tasks can be created statically or dynamically, so note this
                     * task was created statically in case the task is later deleted. */
                    pxNewTCB->ucStaticallyAllocated = tskSTATICALLY_ALLOCATED_STACK_AND_TCB;
                }
                #endif /* tskSTATIC_AND_DYNAMIC_ALLOCATION_POSSIBLE */

                prvInitialiseNewTask( pxTaskCode, pcName, ulStackDepth, pvParameters, uxPriority, &xReturn, pxNewTCB, NULL, xCoreID );
                prvAddNewTaskToReadyList( pxNewTCB );
            }
            else
            {
                xReturn = NULL;
            }
        }

        return xReturn;
    }

#endif /* ( configSUPPORT_STATIC_ALLOCATION == 1 ) */
/*----------------------------------------------------------*/

#if ( configUSE_TIMERS == 1 )

 BaseType_t __attribute__( ( weak ) ) xTimerCreateTimerTask( void )
{
    return pdPASS;
}

#endif /* configUSE_TIMERS */
/*----------------------------------------------------------*/

/* ------------------------------------------------- Task Utilities ------------------------------------------------- */

BaseType_t xTaskGetCoreID( TaskHandle_t xTask )
{
    BaseType_t xReturn;

    TCB_t * pxTCB;

    pxTCB = prvGetTCBFromHandle( xTask );

    xReturn = pxTCB->xCoreID;

    return xReturn;
}
/*----------------------------------------------------------*/

#if ( INCLUDE_xTaskGetIdleTaskHandle == 1 )

    TaskHandle_t xTaskGetIdleTaskHandleForCore( BaseType_t xCoreID )
    {
        /* If xTaskGetIdleTaskHandle() is called before the scheduler has been
         * started, then xIdleTaskHandle will be NULL. */
        configASSERT( taskVALID_CORE_ID( xCoreID ) == pdTRUE );
        configASSERT( ( xIdleTaskHandle[ xCoreID ] != NULL ) );
        return xIdleTaskHandle[ xCoreID ];
    }

#endif /* INCLUDE_xTaskGetIdleTaskHandle */
/*----------------------------------------------------------*/

#if ( ( INCLUDE_xTaskGetCurrentTaskHandle == 1 ) || ( configUSE_MUTEXES == 1 ) )

    TaskHandle_t xTaskGetCurrentTaskHandleForCore( BaseType_t xCoreID )
    {
        TaskHandle_t xReturn;

        configASSERT( taskVALID_CORE_ID( xCoreID ) == pdTRUE );

        {
            /* A critical section is not required as this function does not
             * guarantee that the TCB will still be valid when this function
             * returns. */
            xReturn = pxCurrentTCBs[ xCoreID ];
        }

        return xReturn;
    }

#endif /* ( ( INCLUDE_xTaskGetCurrentTaskHandle == 1 ) || ( configUSE_MUTEXES == 1 ) ) */
/*----------------------------------------------------------*/

#if (1 && ( configGENERATE_RUN_TIME_STATS == 1 ) && ( INCLUDE_xTaskGetIdleTaskHandle == 1 ) )

    configRUN_TIME_COUNTER_TYPE ulTaskGetIdleRunTimeCounterForCore( BaseType_t xCoreID )
    {
        uint32_t ulRunTimeCounter;

        configASSERT( taskVALID_CORE_ID( xCoreID ) == pdTRUE );

        /* For SMP, we need to take the kernel lock here as we are about to
         * access kernel data structures. */
        prvENTER_CRITICAL( &xKernelLock );
        {
            ulRunTimeCounter = xIdleTaskHandle[ xCoreID ]->ulRunTimeCounter;
        }
        /* Release the previously taken kernel lock. */
        prvEXIT_CRITICAL( &xKernelLock );

        return ulRunTimeCounter;
    }

#endif /* (1 && ( configGENERATE_RUN_TIME_STATS == 1 ) && ( INCLUDE_xTaskGetIdleTaskHandle == 1 ) ) */
/*----------------------------------------------------------*/

#if (1 && ( configGENERATE_RUN_TIME_STATS == 1 ) && ( INCLUDE_xTaskGetIdleTaskHandle == 1 ) )

    configRUN_TIME_COUNTER_TYPE ulTaskGetIdleRunTimePercentForCore( BaseType_t xCoreID )
    {
        configRUN_TIME_COUNTER_TYPE ulTotalTime, ulReturn;

        configASSERT( taskVALID_CORE_ID( xCoreID ) == pdTRUE );

        ulTotalTime = portGET_RUN_TIME_COUNTER_VALUE();

        /* For percentage calculations. */
        ulTotalTime /= ( configRUN_TIME_COUNTER_TYPE ) 100;

        /* Avoid divide by zero errors. */
        if( ulTotalTime > ( configRUN_TIME_COUNTER_TYPE ) 0 )
        {
            /* For SMP, we need to take the kernel lock here as we are about
             * to access kernel data structures. */
            prvENTER_CRITICAL( &xKernelLock );
            {
                ulReturn = xIdleTaskHandle[ xCoreID ]->ulRunTimeCounter / ulTotalTime;
            }
            /* Release the previously taken kernel lock. */
            prvEXIT_CRITICAL( &xKernelLock );
        }
        else
        {
            ulReturn = 0;
        }

        return ulReturn;
    }

#endif /* (1 && ( configGENERATE_RUN_TIME_STATS == 1 ) && ( INCLUDE_xTaskGetIdleTaskHandle == 1 ) ) */
/*-----------------------------------------------------------*/

uint8_t * pxTaskGetStackStart( TaskHandle_t xTask )
{
    TCB_t * pxTCB;
    uint8_t * uxReturn;

    pxTCB = prvGetTCBFromHandle( xTask );
    uxReturn = ( uint8_t * ) pxTCB->pxStack;

    return uxReturn;
}
/*----------------------------------------------------------*/

/*----------------------------------------------------------*/

/*----------------------------------------------------------*/

/* --------------------------------------------- TLSP Deletion Callbacks -------------------------------------------- */

#if CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS

    void vTaskSetThreadLocalStoragePointerAndDelCallback( TaskHandle_t xTaskToSet,
                                                          BaseType_t xIndex,
                                                          void * pvValue,
                                                          TlsDeleteCallbackFunction_t pvDelCallback )
    {
        /* If TLSP deletion callbacks are enabled, then configNUM_THREAD_LOCAL_STORAGE_POINTERS
         * is doubled in size so that the latter half of the pvThreadLocalStoragePointers
         * stores the deletion callbacks. */
        if( xIndex < ( configNUM_THREAD_LOCAL_STORAGE_POINTERS / 2 ) )
        {
            TCB_t * pxTCB;

            #if ( configNUM_CORES > 1 )
            {
                /* For SMP, we need a critical section as another core could also
                 * update this task's TLSP at the same time. */
                {
                    taskENTER_CRITICAL( &xKernelLock );
                }
            }
            #endif /* configNUM_CORES > 1 */

            pxTCB = prvGetTCBFromHandle( xTaskToSet );
            /* Store the TLSP by indexing the first half of the array */
            pxTCB->pvThreadLocalStoragePointers[ xIndex ] = pvValue;

            /* Store the TLSP deletion callback by indexing the second half
             * of the array. */
            pxTCB->pvThreadLocalStoragePointers[ ( xIndex + ( configNUM_THREAD_LOCAL_STORAGE_POINTERS / 2 ) ) ] = ( void * ) pvDelCallback;

            #if ( configNUM_CORES > 1 )
            {
                {
                    taskEXIT_CRITICAL( &xKernelLock );
                }
            }
            #endif /* configNUM_CORES > 1 */
        }
    }

#endif /* CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS */

void * pvTaskGetCurrentTCBForCore( BaseType_t xCoreID )
{
    void * pvRet;

    configASSERT( taskVALID_CORE_ID( xCoreID ) == pdTRUE );

    pvRet = ( void * ) pxCurrentTCBs[ xCoreID ];
    return pvRet;
}

