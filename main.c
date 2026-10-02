// Copyright (c) 2024 embeddedboys developers
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <malloc.h>

#include "pico/time.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "pico/platform.h"
#include "pico/stdio_uart.h"

#include "hardware/pll.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/timer.h"

#include "FreeRTOS.h"
#include "task.h"

#include "pud.h"

void vApplicationTickHook()
{
}

/*
 * Two diagnostics the firmware used to lack.
 *
 * The heap is heap_3, so these allocations come from newlib's heap and its
 * bound is the linker script's -- _sbrk stops at 0x20080000.  That bound is
 * enforced, but nothing reported reaching it: a failed xTaskCreate() just
 * returned pdFAIL.  heap_3.c calls this hook on a NULL from malloc when
 * configUSE_MALLOC_FAILED_HOOK is 1, which is visibility without the 16 KB a
 * heap_4 reservation would cost.  mallinfo() reads the same allocator, so the
 * number printed here is the real remainder (notes/pitfalls-memory.md 3.2).
 *
 * Both hooks run where printf is acceptable: the malloc hook from whichever
 * task asked for memory, the stack hook from the context switch that noticed.
 */

static unsigned heap_free_bytes(void)
{
	struct mallinfo mi = mallinfo();

	return (unsigned)mi.fordblks;
}

void vApplicationMallocFailedHook(void)
{
	printf("FATAL: heap exhausted (%u B free)\n", heap_free_bytes());
	__breakpoint();
}

/*
 * A second way to lose a task silently: xTaskCreate() returns
 * errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY instead of panicking, and it allocates
 * the stack and the TCB in two separate mallocs, so the hook above does not
 * necessarily fire for it.  Without this, an over-committed heap shows up as a
 * missing task and a device that half works.
 *
 * The decoder task's creation is inside decoder.c and is not checked here.
 */
static void check_task_created(BaseType_t rc, const char *name)
{
	if (rc != pdPASS) {
		printf("FATAL: xTaskCreate(%s) failed, no memory (%u B free)\n",
		       name, heap_free_bytes());
		__breakpoint();
	}
}

/*
 * configCHECK_FOR_STACK_OVERFLOW = 2.  pcTaskName points into the overflowed
 * task's TCB, which is exactly the memory that may already be damaged -- print
 * it, but do not rely on it being readable or terminated.  main() implements no
 * vApplicationGetIdleTaskMemory/# configSUPPORT_STATIC_ALLOCATION, so the name
 * is the heap-allocated one the kernel copied at creation.
 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
	(void)xTask;
	printf("FATAL: stack overflow in task '%s'\n",
	       pcTaskName ? pcTaskName : "(unnamed)");
	__breakpoint();
}

#if !INDEV_DRV_NOT_USED
/*
 * Touch polling.  There is deliberately no printf here: at 115200 baud a line
 * costs 1-3 ms, the poll period is 33 ms, and the coordinates are what EP4 is
 * for -- scripts/touch_test.py on the host shows them.
 */
portTASK_FUNCTION(example_indev_read_task, pvParameters)
{
	indev_driver_init();

	for (;;) {
		pud_touch_poll();
		vTaskDelay(pdMS_TO_TICKS(g_pud_data.tp.polling_period));
	}

	vTaskDelete(NULL);
}
#endif

static portTASK_FUNCTION(usb_task_handler, pvParameters)
{
	usb_device_init();

	/*
	 * Every task and the timer queue are allocated by now, so this is the
	 * steady-state heap headroom, read from the same allocator the kernel
	 * uses.  It is printed rather than assumed: there is no
	 * configTOTAL_HEAP_SIZE to compare against (heap_3 ignores it), and a
	 * number nobody can read is how the 128 KB it used to claim went on
	 * being believed.  Watch it if a decoder or a buffer changes.
	 */
	printf("heap after init: %u B free\n", heap_free_bytes());

	while (!usb_is_configured())
		vTaskDelay(pdMS_TO_TICKS(1));

	vTaskSuspend(NULL);
}

int main(void)
{
	/* NOTE: DO NOT MODIFY THIS BLOCK */
#define CPU_SPEED_MHZ (DEFAULT_SYS_CLK_KHZ / 1000)
	if (CPU_SPEED_MHZ > 266 && CPU_SPEED_MHZ <= 360)
		vreg_set_voltage(VREG_VOLTAGE_1_20);
	else if (CPU_SPEED_MHZ > 360 && CPU_SPEED_MHZ <= 396)
		vreg_set_voltage(VREG_VOLTAGE_1_25);
	else if (CPU_SPEED_MHZ > 396)
		vreg_set_voltage(VREG_VOLTAGE_MAX);
	else
		vreg_set_voltage(VREG_VOLTAGE_DEFAULT);

	busy_wait_at_least_cycles(
	        (uint32_t)((SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST_DELAY_US *
	                    (uint64_t)XOSC_HZ) /
	                   1000000));

	set_sys_clock_khz(CPU_SPEED_MHZ * 1000, true);
	clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
	                CPU_SPEED_MHZ * MHZ, CPU_SPEED_MHZ * MHZ);
	stdio_uart_init_full(debug_uart, DEBUG_UART_SPEED, DEBUG_UART_TX_PIN,
	                     DEBUG_UART_RX_PIN);

	printf("\n\n\nPICO USB Display\n");
	printf("CPU clockspeed: %d MHz\n", CPU_SPEED_MHZ);

	pud_init();

	TaskHandle_t usb_handler;
	check_task_created(xTaskCreate(usb_task_handler, "usb_task", 256, NULL,
	                               (tskIDLE_PRIORITY + 3), &usb_handler),
	                   "usb_task");
	vTaskCoreAffinitySet(usb_handler, (1 << 0));

#if !INDEV_DRV_NOT_USED
	TaskHandle_t indev_handler;
	check_task_created(xTaskCreate(example_indev_read_task, "indev_read", 256,
	                               NULL, (tskIDLE_PRIORITY + 0),
	                               &indev_handler),
	                   "indev_read");
#endif

	printf("calling freertos scheduler, %lld\n", time_us_64());
	vTaskStartScheduler();
	for (;;)
		;

	return 0;
}
