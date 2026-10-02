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
#include "hardware/watchdog.h"

#include "FreeRTOS.h"
#include "task.h"

#include "pud.h"

/*
 * Self-healing watchdog.
 *
 * The device has to recover on its own: a panel that stops updating while the
 * host's writes time out is not something a person can fix without a power
 * cycle, and the failure is rare enough (about 1 in 15000 sends, notes/qoid.md)
 * that it will be met in the field rather than at a bench.
 *
 * It has to be a supervisor *outside* the task that gets stuck.  The failure it
 * exists for is the decoder task blocking inside a panel transfer -- the one
 * wait on that path without a timeout is dma_channel_wait_for_finish_blocking()
 * -- and a watchdog inside that task can never run.  This task sits above every
 * application task (decoder is idle+1, usb idle+3), so the tick still schedules
 * it while the decoder spins.
 *
 * What it watches is work that is outstanding but not finishing: `submitted >
 * drawn` unchanged for PUD_STALL_MS.  When the device is idle the two are
 * equal, so an idle panel is not a stall, and no knowledge of the frame-slot
 * count is needed (that constant is private to decoder.c and copying it here
 * would be one more thing to keep in step).
 *
 * Recovery is the chip's hardware watchdog, not a state reset this task would
 * have to do on a task that may be holding the panel bus mid-transfer.  On a
 * stall the supervisor records why and stops feeding; the reset follows.  The
 * record lives in watchdog scratch registers, which survive the reset, so the
 * next boot can say what happened instead of looking like a random reboot.
 *
 * Scratch slots: 4 belongs to the SDK (watchdog_enable_caused_reboot reads it).
 * 0 and 1 are ours.  The coremark repository uses 6 and 7 for its own counter;
 * these are different firmwares, but the numbers are written down here so the
 * two are not confused when a debugger is pointed at either.
 */
#define PUD_WD_MAGIC_SLOT 0
#define PUD_WD_COUNT_SLOT 1
#define PUD_WD_MAGIC 0x50554457u /* "PUDW" */

/* 4 s: long enough that a slow-but-working frame is never mistaken for a hang,
 * short enough that a frozen panel comes back before a person reaches the
 * power.  The stall threshold is a quarter of it, and that in turn is about
 * thirteen times the longest measured frame (full screen, 73.7 ms). */
#define PUD_WATCHDOG_MS 4000u
#define PUD_STALL_MS 1000u
#define PUD_SUPERVISOR_PERIOD_MS 250u

/* Owned by decoder.c.  Declared here rather than in a header because the
 * supervisor is the only reader outside that file.
 *
 * `dropped` is needed, not just submitted and drawn.  decoder_submit_frame()
 * increments `submitted` *before* it looks for a free slot, so a frame that is
 * then dropped counts as submitted and never as drawn -- and "submitted >
 * drawn" would stay true for the rest of the boot after a single drop.  The
 * supervisor would read that as a 1 s stall the first time the panel went
 * idle, i.e. it would reset a working device.  Outstanding work is therefore
 * submitted - dropped - drawn, which is zero whenever the pipeline is empty. */
extern volatile u32 g_decoder_stat_submitted;
extern volatile u32 g_decoder_stat_drawn;
extern volatile u32 g_decoder_stat_dropped;

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
 * The supervisor described at the top of this file.  Every period it either
 * feeds the hardware watchdog or decides that the pipeline has stopped, and on
 * a stall it stops feeding and lets the chip reset.
 */
static portTASK_FUNCTION(watchdog_supervisor_task, pvParameters)
{
	u32 last_drawn = 0;
	u32 stuck_ms = 0;
	u32 stalls = (u32)watchdog_hw->scratch[PUD_WD_COUNT_SLOT];

	for (;;) {
		u32 drawn = g_decoder_stat_drawn;
		u32 submitted = g_decoder_stat_submitted;
		u32 dropped = g_decoder_stat_dropped;
		u32 outstanding = submitted - dropped - drawn;

		if (drawn != last_drawn) {
			last_drawn = drawn;
			stuck_ms = 0;
		} else if (outstanding) {
			/* Work is outstanding and none of it is finishing. */
			stuck_ms += PUD_SUPERVISOR_PERIOD_MS;
		} else {
			/* Nothing queued: the healthy steady state, whether or
			 * not frames were dropped earlier. */
			stuck_ms = 0;
		}

		if (stuck_ms >= PUD_STALL_MS) {
			watchdog_hw->scratch[PUD_WD_MAGIC_SLOT] = PUD_WD_MAGIC;
			watchdog_hw->scratch[PUD_WD_COUNT_SLOT] = stalls + 1;
			printf("watchdog: decoder stalled for %u ms with %u of %u "
			       "drawn (%u dropped) -- not feeding any more, the "
			       "chip will reset (recovery %u)\n",
			       (unsigned)stuck_ms, (unsigned)drawn,
			       (unsigned)submitted, (unsigned)dropped,
			       (unsigned)(stalls + 1));
			/* Deliberately stop calling watchdog_update().  Resetting
			 * state that a task blocked mid-transfer may be holding
			 * is not something this task can do safely; the chip's
			 * reset is, and it also covers a hang this task cannot
			 * see. */
			for (;;)
				vTaskDelay(pdMS_TO_TICKS(PUD_WATCHDOG_MS));
		}

		watchdog_update();
		vTaskDelay(pdMS_TO_TICKS(PUD_SUPERVISOR_PERIOD_MS));
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

	/*
	 * Above every application task, so it still runs when one of them is
	 * spinning: the decoder is idle+1 and the supervisor is idle+4.
	 */
	check_task_created(xTaskCreate(watchdog_supervisor_task, "supervisor", 256,
	                               NULL, (tskIDLE_PRIORITY + 4), NULL),
	                   "supervisor");

	/*
	 * A recovery from a previous boot is reported before arming, because
	 * arming overwrites the SDK's own marker in scratch[4].  Our scratch
	 * slots survive the reset, so a stall that healed itself is visible as
	 * such instead of looking like an unexplained reboot.
	 */
	if (watchdog_hw->scratch[PUD_WD_MAGIC_SLOT] == PUD_WD_MAGIC)
		printf("watchdog: recovered from %u decoder stall(s) in a previous "
		       "boot\n",
		       (unsigned)watchdog_hw->scratch[PUD_WD_COUNT_SLOT]);
	else if (watchdog_caused_reboot())
		printf("watchdog: last boot was reset by the watchdog\n");

	/* pause_on_debug: a breakpoint must not be interrupted by a reset. */
	watchdog_enable(PUD_WATCHDOG_MS, true);

	printf("calling freertos scheduler, %lld\n", time_us_64());
	vTaskStartScheduler();
	for (;;)
		;

	return 0;
}
