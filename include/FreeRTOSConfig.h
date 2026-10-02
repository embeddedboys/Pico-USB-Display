// Kept in the style of the FreeRTOS template it comes from: the option values
// are aligned into a column, which the kernel .clang-format would take apart.
// clang-format off
/*
 * FreeRTOS V202111.00
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://www.FreeRTOS.org
 * http://aws.amazon.com/freertos
 *
 * 1 tab == 4 spaces!
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/*-----------------------------------------------------------
 * Application specific definitions.
 *
 * These definitions should be adjusted for your particular hardware and
 * application requirements.
 *
 * THESE PARAMETERS ARE DESCRIBED WITHIN THE 'CONFIGURATION' SECTION OF THE
 * FreeRTOS API DOCUMENTATION AVAILABLE ON THE FreeRTOS.org WEB SITE.
 *
 * See http://www.freertos.org/a00110.html
 *----------------------------------------------------------*/

/* Scheduler Related */
#define configUSE_PREEMPTION                    1
#define configUSE_TICKLESS_IDLE                 0
#define configUSE_IDLE_HOOK                     0
#define configUSE_PASSIVE_IDLE_HOOK             0
#define configUSE_TICK_HOOK                     0
#define configUSE_MINIMAL_IDLE_HOOK             0
#define configTICK_RATE_HZ                      ( ( TickType_t ) 1000 )
#define configMAX_PRIORITIES                    32
#define configMINIMAL_STACK_SIZE                ( configSTACK_DEPTH_TYPE ) 256
#define configUSE_16_BIT_TICKS                  0

#define configIDLE_SHOULD_YIELD                 1

/* Synchronization Related */
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_APPLICATION_TASK_TAG          0
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               8
#define configUSE_QUEUE_SETS                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_NEWLIB_REENTRANT              0
// todo need this for lwip FreeRTOS sys_arch to compile
#define configENABLE_BACKWARD_COMPATIBILITY     1
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 5

/* System */
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE        size_t

/* Memory allocation related definitions. */
#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1

/* This number does nothing, and saying so here is the point: the linked heap is
 * heap_3, which only wraps malloc() and never reads this constant (there is no
 * ucHeap symbol).  The kernel's allocations therefore come from the C library's
 * heap, whose real bound is the linker script's -- _sbrk refuses to pass
 * 0x20080000, so exhaustion returns NULL instead of growing into the interrupt
 * stack.  Left at a round value rather than deleted, because a future move to
 * heap_4 or heap_5 would make it live again.
 *
 * What was actually missing was visibility, not a bound, so the fix is
 * configUSE_MALLOC_FAILED_HOOK below and the hook in main.c -- which heap_3.c
 * does call (portable/MemMang/heap_3.c).  See notes/pitfalls-memory.md 3.2.
 * For the kernel's needs, if it ever does move to heap_4: the application never
 * calls pvPortMalloc(), CherryUSB is built without an OSAL (src/cherryusb/
 * CMakeLists.txt lists no usb_osal_*.c and no usb_osal_* symbol is in the ELF),
 * and the tasks total about 10.3 KB with their TCBs and one binary semaphore. */
#define configTOTAL_HEAP_SIZE                   (128*1024)
#define configAPPLICATION_ALLOCATED_HEAP        0

/* Hook function related definitions. */
/* Method 2 (check the pattern at every context switch).  This firmware has a
 * history of HardFaults from an overflowing interrupt stack -- CFSR STKERR,
 * see notes/debugging.md -- and the decoder path's peak is a measured 632 B
 * against a 4 KB task stack, which is only safe while someone keeps checking.
 * A silent overflow corrupts whatever is next in RAM; the hook below names the
 * task instead. */
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0

/* Run time and task stats gathering related definitions. */
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

/* Co-routine related definitions. */
#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         1

/* Software timer related definitions. */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               ( configMAX_PRIORITIES - 1 )
#define configTIMER_QUEUE_LENGTH                10
/* 256 words = 1 KB. The firmware creates no software timers, so the kernel
 * task exists only because configUSE_TIMERS is 1. The old 1024 words wasted
 * 3 KB of heap. Measured idle usage of the timer task is 152 B; raise this
 * again before putting real work into a timer callback. */
#define configTIMER_TASK_STACK_DEPTH            256

/* Interrupt nesting behaviour configuration. */
/*
#define configKERNEL_INTERRUPT_PRIORITY         [dependent of processor]
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    [dependent on processor and application]
#define configMAX_API_CALL_INTERRUPT_PRIORITY   [dependent on processor and application]
*/

#if FREE_RTOS_KERNEL_SMP // set by the RP2040 SMP port of FreeRTOS
/* SMP port only */
#define configNUMBER_OF_CORES                   2
#define configTICK_CORE                         1
#define configRUN_MULTIPLE_PRIORITIES           1
#define configUSE_CORE_AFFINITY                 1
#endif

/* RP2040 specific */
#define configSUPPORT_PICO_SYNC_INTEROP         1
#define configSUPPORT_PICO_TIME_INTEROP         1

/* RP2350 grows some features */
#define configENABLE_FPU                        1
#define configENABLE_MPU                        0
#define configENABLE_TRUSTZONE                  0
#define configRUN_FREERTOS_SECURE_ONLY          1
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    16

#include <assert.h>
/* Define to trap errors during development. */
#define configASSERT(x)                         assert(x)

// #include <stdio.h>
/* Define to trap errors during development. */
// #define configASSERT(x)                         \
//     do { \
//         if(!(x)) \
//             printf("[assert]: "#x"\n"); \
//     } while(0);
#define configTASK_NOTIFICATION_ARRAY_ENTRIES    2

/* Set the following definitions to 1 to include the API function, or zero
to exclude the API function. */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_xTaskAbortDelay                 1
#define INCLUDE_xTaskGetHandle                  1
#define INCLUDE_xTaskResumeFromISR              1
#define INCLUDE_xQueueGetMutexHolder            1

/* A header file that defines trace macro can be included here. */

#endif /* FREERTOS_CONFIG_H */
