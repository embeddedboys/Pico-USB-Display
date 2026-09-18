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

#define PUD_CMD_GET_SN	0x01
#define PUD_CMD_GET_CAPS	0x02

/* Device capabilities, answered by PUD_CMD_GET_CAPS on the EP2 IN path.  The
 * host needs frame_max to size the bands it splits a rectangle into: the same
 * number sizes ep1_read_buffer and the decoder frame slot on the device side
 * (PUD_MAX_TRANSFER, see usbd_vendor.h) and it differs per board (RP2040 has
 * half the SRAM).  Keep this struct in sync with the driver's pud.h. */
#define PUD_CAPS_MAGIC	0x43445550 /* "PUDC" */
#define PUD_PROTO_VER	1

struct pud_caps {
	u32	magic;
	u32	proto_ver;
	u32	frame_max;	/* max bytes the device accepts in one EP1 transfer */
	u32	decoder_type;	/* 0 tjpgd, 1 JPEGDEC, 2 LZ4, 3 QOI */
};

struct disp_data {
	u16	xres;
	u16	yres;
	u8	rotation;
	u32	pixelclock_khz;
	u8	bpp;
	u8	intf_type;
};

struct tp_data {
	bool	is_pressed;
	u16	x;
	u16	y;
	u8	polling_period;
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
