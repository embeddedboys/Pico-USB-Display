// Copyright (c) 2025 embeddedboys developers
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
#include <string.h>
#include "pico/unique_id.h"

#include "pud.h"

struct pud_data g_pud_data = {0};

#define define_pud_rw_attr(field, field_type)		\
void pud_get_rw_##field(field_type *ptr, int len)	\
{							\
	struct pud_data *data = &g_pud_data;		\
	memcpy((void *)ptr, &data->field, len);		\
}							\
void pud_set_rw_##field(field_type *ptr, int len)	\
{							\
	struct pud_data *data = &g_pud_data;		\
	memcpy(&data->field, (void *)ptr, len);		\
}

#define define_pud_ro_attr(field, field_type)		\
void pud_get_ro_##field(field_type *ptr, int len)	\
{							\
	struct pud_data *data = &g_pud_data;		\
	memcpy((void *)ptr, &data->field, len);		\
}

#define define_pud_rw_sub_attr(parent, field, field_type)	\
void pud_get_rw_##parent##_##field(field_type *ptr, int len)	\
{								\
	struct pud_data *data = &g_pud_data;			\
	memcpy((void *)ptr, &data->parent.field, len);		\
}								\
void pud_set_rw_##parent##_##field(field_type *ptr, int len)	\
{								\
	struct pud_data *data = &g_pud_data;			\
	memcpy(&data->parent.field, (void *)ptr, len);		\
}

#define define_pud_ro_sub_attr(parent, field, field_type)	\
void pud_get_ro_##parent##_##field(field_type *ptr, int len)	\
{								\
	struct pud_data *data = &g_pud_data;			\
	memcpy((void *)ptr, &data->parent.field, len);		\
}

define_pud_rw_attr(cmd, u8);
define_pud_ro_attr(sn, u8);

define_pud_ro_sub_attr(disp, xres, u16);
define_pud_ro_sub_attr(disp, yres, u16);
define_pud_ro_sub_attr(disp, rotation, u8);
define_pud_ro_sub_attr(disp, pixelclock_khz, u16);
define_pud_ro_sub_attr(disp, bpp, u8);
define_pud_ro_sub_attr(disp, intf_type, u8);

define_pud_rw_sub_attr(tp, polling_period, u8);

static void pud_hexdump(void *ptr, int len)
{
	char *data = (char *)ptr;
	int i;

	for (i = 0; i < len; i++) {
		if (i % 16 == 0)
			printf("%08x  ", i);

		printf("%02x ", data[i]);

		if (i % 16 == 7)
			printf(" ");

		if ((i + 1) % 16 == 0)
			printf("\n");
	}
	printf("\n");
}

static void pud_show_debug_info(void)
{
	char id_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];
	pico_get_unique_board_id_string(id_str, sizeof(id_str));
	printf("Unique ID: %s\n", id_str);

	u8 sn[8];
	pud_get_ro_sn(sn, ARRAY_SIZE(sn));
	pud_hexdump(sn, ARRAY_SIZE(sn));

	u16 xres, yres, pixelclock_khz;
	pud_get_ro_disp_xres(&xres, sizeof(xres));
	pud_get_ro_disp_yres(&yres, sizeof(yres));
	pud_get_ro_disp_pixelclock_khz(&pixelclock_khz, sizeof(pixelclock_khz));
	printf("%s, xres : %d, yres : %d, pixelclock_khz : %d\n",
		__func__, xres, yres, pixelclock_khz);
}

static void pud_config_init(void)
{
	struct pud_data *data = &g_pud_data;
	pico_unique_board_id_t id;

	pico_get_unique_board_id(&id);
	memcpy(data->sn, id.id, PICO_UNIQUE_BOARD_ID_SIZE_BYTES);

	data->disp.xres = TFT_HOR_RES;
	data->disp.yres = TFT_VER_RES;
	data->disp.rotation = TFT_ROTATION;
	data->disp.intf_type = 0;
	data->disp.pixelclock_khz = TFT_BUS_CLK_KHZ;

	data->tp.polling_period = INDEV_POLLING_PERIOD_MS;
}

/*
 * One touch poll.
 *
 * Keeps g_pud_data.tp current and pushes a report when there is something to
 * say: while the panel is held (so moves arrive) and once on release.  Nothing
 * is sent while it is idle -- the host's interrupt URB simply stays pending,
 * which is how an input endpoint normally works.
 */
void pud_touch_poll(void)
{
	static bool was_pressed;
	static u8 seq;
	struct pud_data *data = &g_pud_data;
	struct pud_touch_report report;
	bool pressed = indev_is_pressed();
	u16 x = 0, y = 0;

	if (pressed) {
		/*
		 * indev_read_*() already returns panel coordinates in the display
		 * frame: the library applies the transform for the display's
		 * rotation (TFT_ROTATION, the same number the panel is driven
		 * with).  Clamp anyway -- the report is a wire contract, and a
		 * finger resting on the bezel can read past the active area.
		 */
		x = indev_read_x();
		y = indev_read_y();
		if (x >= TFT_HOR_RES)
			x = TFT_HOR_RES - 1;
		if (y >= TFT_VER_RES)
			y = TFT_VER_RES - 1;
	}

	data->tp.is_pressed = pressed;
	data->tp.x = x;
	data->tp.y = y;

	if (!pressed && !was_pressed)
		return;		/* idle, and it already knows */

	was_pressed = pressed;

	report.flags = pressed ? PUD_TOUCH_PRESSED : 0;
	report.x_hi = (u8)(x >> 8);
	report.x_lo = (u8)(x & 0xff);
	report.y_hi = (u8)(y >> 8);
	report.y_lo = (u8)(y & 0xff);
	report.seq = ++seq;
	report.version = PUD_TOUCH_VERSION;
	report.reserved = 0;

	usbd_vendor_ep4_submit(&report);
}

void pud_init(void)
{
	tft_driver_init();
	backlight_driver_init();
	decoder_init();

	pud_config_init();

	pud_show_debug_info();
}
