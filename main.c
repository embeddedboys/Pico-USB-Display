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
#include "semphr.h"

#include "usb.h"
#include "tft.h"
#include "indev.h"
#include "config.h"
#include "backlight.h"
#include "decoder.h"

#include "bootlogo.h"

u32 frame_counter = 0;
// QueueHandle_t xToFlushQueue = NULL;

void vApplicationTickHook()
{

}

static portTASK_FUNCTION(bootlogo_task_handler, pvParameters)
{
    decoder_drawimg(0, 0, TFT_HOR_RES - 1, TFT_VER_RES - 1,
                    (uint8_t *)bootlogo, sizeof(bootlogo));

    busy_wait_ms(10);
    backlight_driver_init();
    backlight_set_level(100);
    printf("backlight set to 100%%\n");

    vTaskDelete(NULL);
}

#if !INDEV_DRV_NOT_USED
portTASK_FUNCTION(example_indev_read_task, pvParameters)
{
    indev_driver_init();

    for (;;) {
        if (indev_is_pressed())
            printf("pressed at (%d, %d)\n", indev_read_x(), indev_read_y());

        vTaskDelay(pdMS_TO_TICKS(INDEV_POLLING_PERIOD_MS));
    }

    vTaskDelete(NULL);
}
#endif

static portTASK_FUNCTION(usb_task_handler, pvParameters)
{
    usb_device_init();

    while (!usb_is_configured());

    for(;;) tight_loop_contents();
}

int main(void)
{
    /* NOTE: DO NOT MODIFY THIS BLOCK */
#define CPU_SPEED_MHZ (DEFAULT_SYS_CLK_KHZ / 1000)
    if(CPU_SPEED_MHZ > 266 && CPU_SPEED_MHZ <= 360)
        vreg_set_voltage(VREG_VOLTAGE_1_20);
    else if (CPU_SPEED_MHZ > 360 && CPU_SPEED_MHZ <= 396)
        vreg_set_voltage(VREG_VOLTAGE_1_25);
    else if (CPU_SPEED_MHZ > 396)
        vreg_set_voltage(VREG_VOLTAGE_MAX);
    else
        vreg_set_voltage(VREG_VOLTAGE_DEFAULT);

    busy_wait_at_least_cycles((uint32_t)((SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST_DELAY_US * (uint64_t)XOSC_HZ) / 1000000));

    set_sys_clock_khz(CPU_SPEED_MHZ * 1000, true);
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    CPU_SPEED_MHZ * MHZ,
                    CPU_SPEED_MHZ * MHZ);
    stdio_uart_init_full(debug_uart, DEBUG_UART_SPEED,
    			DEBUG_UART_TX_PIN, DEBUG_UART_RX_PIN);
    // stdio_uart_init();

    printf("\n\n\nPICO USB Display\n");

    printf("CPU clockspeed: %d MHz\n", CPU_SPEED_MHZ);

    tft_driver_init();
    backlight_driver_init();
    decoder_init();

    // xToFlushQueue = xQueueCreate(1, sizeof(struct video_frame));
    // TaskHandle_t video_push_handler;
    // xTaskCreate(example_video_push_task, "video_push", 256, NULL, (tskIDLE_PRIORITY + 1), &video_push_handler);
    // vTaskCoreAffinitySet(video_push_handler, (1 << 0));

    TaskHandle_t usb_handler;
    xTaskCreate(usb_task_handler, "usb_task", 256, NULL, (tskIDLE_PRIORITY + 3), &usb_handler);
    vTaskCoreAffinitySet(usb_handler, (1 << 0));

    TaskHandle_t bootlogo_handler;
    xTaskCreate(bootlogo_task_handler, "bootlogo_task", 256, NULL, (tskIDLE_PRIORITY + 2), &bootlogo_handler);
    vTaskCoreAffinitySet(bootlogo_handler, (1 << 1));

#if !INDEV_DRV_NOT_USED
    TaskHandle_t indev_handler;
    xTaskCreate(example_indev_read_task, "indev_read", 256, NULL, (tskIDLE_PRIORITY + 0), &indev_handler);
#endif

    printf("calling freertos scheduler, %lld\n", time_us_64());
    vTaskStartScheduler();
    for(;;);

    return 0;
}
