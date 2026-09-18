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

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

typedef signed char s8;
typedef signed short s16;
typedef signed int s32;

/* Panel active area in mm, set by the build (see CMakeLists.txt).  0 means
 * "unknown", which the host reads as "leave the input resolution alone". */
#ifndef PUD_PANEL_WIDTH_MM
#define PUD_PANEL_WIDTH_MM 0
#endif
#ifndef PUD_PANEL_HEIGHT_MM
#define PUD_PANEL_HEIGHT_MM 0
#endif

#define PUD_CMD_GET_SN 0x01
#define PUD_CMD_GET_CAPS 0x02

/* Device capabilities, answered by PUD_CMD_GET_CAPS on the EP2 IN path.  The
 * host needs frame_max to size the bands it splits a rectangle into: the same
 * number sizes ep1_read_buffer and the decoder frame slot on the device side
 * (PUD_MAX_TRANSFER, see usbd_vendor.h) and it differs per board (RP2040 has
 * half the SRAM).  Keep this struct in sync with the driver's pud.h and with
 * scripts/pud_usb.py.
 *
 * The panel parameters after decoder_type were appended later: a host that
 * asks for the first 16 bytes (2.0) still gets a valid answer, because the
 * device clamps its reply to the requested length. */
#define PUD_CAPS_MAGIC 0x43445550 /* "PUDC" */
#define PUD_PROTO_VER 2

struct pud_caps {
	u32 magic;
	u32 proto_ver;
	u32 frame_max; /* max bytes per EP1 transfer, header included */
	u32 decoder_type; /* 0 tjpgd, 1 JPEGDEC, 2 LZ4, 3 QOI, 4 RLE */

	u16 xres; /* panel size in the frame it is driven in */
	u16 yres;
	u16 pixelclock_khz; /* bus clock the panel is driven with */
	u8 rotation; /* TFT_ROTATION the firmware applied */
	u8 bpp;
	u8 intf_type;
	u8 tp_polling_period; /* touch poll period, ms (0 when there is no touch) */
	u16 width_mm; /* active area, for the host's input resolution */
	u16 height_mm;
	u16 flags; /* PUD_CAPS_*: what this build actually has */
};

/*
 * Capability flags.  Touch is optional: most board configs in
 * pico-display-lib set INDEV_DRV_NOT_USED=1 (no controller on the glass), and
 * the host must not register an input device for those.
 */
#define PUD_CAPS_TOUCH 0x0001 /* an indev driver is compiled in and polled */

/*
 * EP1 OUT framing (protocol v2).
 *
 * Every transfer is a header followed by the payload it describes:
 *
 *     [ struct pud_ep1_header ][ payload ]
 *
 * The rectangle and the length travel with the data instead of arriving first
 * in a REQ_EP1_OUT control request.  That removes one control transfer per
 * band: measured 0.14 ms on an idle bus, and on a full-speed link a control
 * transfer can cost a whole frame once the bulk stream is saturating it.
 *
 * The device reads one max-size packet first, which always holds the whole
 * header (PUD_EP1_HEADER_SIZE <= 64), and then exactly the remaining payload,
 * so the end of a transfer never depends on a short packet.  A host that
 * declares more than frame_max - PUD_EP1_HEADER_SIZE gets its endpoint stalled
 * instead of a truncated frame (g_ep1_stat_oversize).
 */
#define PUD_EP1_HEADER_SIZE 12

struct pud_ep1_header {
	u16 xs;
	u16 ys;
	u16 xe;
	u16 ye;
	u32 size; /* payload bytes that follow */
};

struct disp_data {
	u16 xres;
	u16 yres;
	u8 rotation;
	u32 pixelclock_khz;
	u8 bpp;
	u8 intf_type;
	/* Active area in mm.  The host needs it to give the input device a
	 * resolution (units/mm): libinput treats an absolute device without one
	 * as a kernel bug, and Mutter uses the device size when it decides which
	 * output a touchscreen belongs to. */
	u16 width_mm;
	u16 height_mm;
};

struct tp_data {
	bool is_pressed;
	u16 x;
	u16 y;
	u8 polling_period;
};

/*
 * EP4 touch report, one per interrupt IN transfer (see notes/usb-protocol.md).
 *
 * The layout is byte-explicit on purpose: no multi-byte fields, so neither end
 * depends on alignment or packing rules, and the first five bytes are the ones
 * the driver parsed before the device actually reported anything (flags,
 * big-endian x/y).  A device that has never been touched answers with an
 * all-zero report, which reads as "not pressed".
 *
 *   0  flags      bit0 = pressed
 *   1  x >> 8     panel coordinates in the frame the panel is driven in
 *   2  x & 0xff
 *   3  y >> 8     0..TFT_VER_RES-1
 *   4  y & 0xff
 *   5  sequence   wraps; a jump means the host missed (coalesced) a report
 *   6  version    PUD_TOUCH_VERSION, so a host can tell touch is implemented
 *   7  reserved   0
 *
 * The device pushes a report per poll while the panel is held, plus one on
 * release; the host keeps an interrupt URB pending.  REQ_EP4_IN still exists
 * (and pulls the current report) for hosts that poll instead.
 */
#define PUD_TOUCH_VERSION 1
#define PUD_TOUCH_PRESSED 0x01

struct pud_touch_report {
	u8 flags;
	u8 x_hi;
	u8 x_lo;
	u8 y_hi;
	u8 y_lo;
	u8 seq;
	u8 version;
	u8 reserved;
};

struct pud_data {
	u8 sn[8];
	u16 cmd;

	struct disp_data disp;
	struct tp_data tp;
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

/* One touch poll: refresh g_pud_data.tp and push an EP4 report if there is
 * something to say.  Called from the indev task every polling_period ms. */
void pud_touch_poll(void);

#endif /* __PUD_H */
