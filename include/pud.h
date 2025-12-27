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

#ifndef __PUD_H
#define __PUD_H

#include "usb.h"
#include "tft.h"
#include "indev.h"
#include "config.h"
#include "backlight.h"
#include "decoder.h"

typedef unsigned char	u8;
typedef unsigned short	u16;
typedef unsigned int	u32;

typedef signed char	s8;
typedef signed short	s16;
typedef signed int	s32;

#define pud_CMD_GET_SN	0x01

struct disp_data {
	u16	xres;
	u16	yres;
	u8	rotation;
	u16	pixelclock_khz;
	u8	bpp;
	u8	intf_type;
};

struct tp_data {
	bool	is_pressed;
	u16	x;
	u16	y;
	u8	polling_period;
};

struct tjpgd_data {
	const u8	*array_data;
	u32		array_index;
	u32		array_size;
	s16		jpeg_x;
	s16		jpeg_y;
};

struct jpegdec_data {
	JPEGIMAGE	img;
	u8		options;
};

struct decoder_data {

	u8	type;
};

struct pud_data {
	u8	sn[8];
	u16	cmd;

	struct disp_data	disp;
	struct tp_data		tp;
	struct decoder_data	decoder;
};

extern struct pud_data g_pud_data;

void pud_init(void);

void pud_get_ro_sn(u8 *ptr, int len);

void pud_set_rw_cmd(u8 *ptr, int len);
void pud_get_rw_cmd(u8 *ptr, int len);

void pud_get_ro_disp_xres(u16 *ptr, int len);
void pud_get_ro_disp_yres(u16 *ptr, int len);
void pud_get_ro_disp_rotation(u8 *ptr, int len);
void pud_get_ro_disp_pixelclock_khz(u16 *ptr, int len);
void pud_get_ro_disp_bpp(u8 *ptr, int len);
void pud_get_ro_disp_intf_type(u8 *ptr, int len);

void pud_get_rw_tp_polling_period(u8 *ptr, int len);
void pud_set_rw_tp_polling_period(u8 *ptr, int len);


#endif	/* __PUD_H */
