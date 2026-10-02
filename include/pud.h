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
#define PUD_CMD_SET_PARAM 0x03 /* control OUT (REQ_SET_PARAM), struct pud_params */
#define PUD_CMD_GET_PARAM 0x04 /* EP2 IN, struct pud_param_state */
/* EP2 IN, struct pud_qoid_state: which band each DECODER_TYPE 6 dictionary
 * window holds.  A new command rather than a field appended to
 * struct pud_caps -- the caps struct is a protocol field the driver already
 * parses at a fixed size, and an unknown command is simply never sent.
 *
 * NOT ANSWERED YET: the handler existed and hung the pipeline on hardware, so
 * it was pulled; the shapes are kept because they are the design.  See
 * notes/todo.md item 13 before wiring it up. */
#define PUD_CMD_GET_QOID 0x05

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
	u32 decoder_type; /* 0 tjpgd, 1 JPEGDEC, 2 LZ4, 3 QOI, 4 RLE,
	                     5 QOI+deflate (experimental) */

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
 * The wire size, stated where the compiler can check it.  `struct pud_caps` is
 * a protocol field: the host unpacks a fixed layout, and a field inserted in
 * the middle (rather than appended) would silently shift every value after it.
 * 32 is what both sides ship today; the first 16 bytes are the pre-panel-field
 * layout an older host still reads.  tests/test_protocol_constants.py compares
 * this with the host mirror's struct.Struct size.
 */
#define PUD_CAPS_SIZE 32
_Static_assert(sizeof(struct pud_caps) == PUD_CAPS_SIZE,
               "pud_caps wire size (append only, never reorder)");

/*
 * Capability flags.  Touch is optional: most board configs in
 * pico-display-lib set INDEV_DRV_NOT_USED=1 (no controller on the glass), and
 * the host must not register an input device for those.
 */
#define PUD_CAPS_TOUCH 0x0001 /* an indev driver is compiled in and polled */

/*
 * Runtime parameters (protocol v2, appended without a version bump: an old host
 * never sends these commands, and an old device answers GET_PARAM with a short
 * read, which a host has to read as "unsupported" -- the same rule the query
 * path already uses for unknown commands).
 *
 * The host writes a mask plus the values it wants; the device answers a query
 * with the values in effect, the set it can change at runtime, and what the last
 * write could not apply.  Reporting the mask instead of stalling keeps the two
 * sides compatible while the parameter set grows: a build that cannot do
 * `rotation` yet says so, and the host can decide what to do about it.
 *
 * Keep this in sync with the driver's pud.h and with scripts/pud_usb.py.
 */
#define PUD_PARAM_BRIGHTNESS 0x00000001 /* u8, 0..100 percent */
#define PUD_PARAM_ROTATION 0x00000002 /* 0..3, TFT_ROTATION numbering */
/* 0x00000004 is retired: it was `fps`, and the device does not pace frames at
 * all -- EP1 flow control makes the host wait, which cannot drop a frame.  The
 * bit stays unused instead of being renumbered, the same rule the request
 * numbers follow: an implementation that already knows it must not silently
 * start meaning something else. */
#define PUD_PARAM_DECODER 0x00000008 /* 0..5, DECODER_TYPE numbering */

struct pud_params {
	u32 mask; /* PUD_PARAM_*: which of the values below this write sets */
	u8 brightness; /* 0..100 percent (the backlight takes a percentage) */
	u8 rotation; /* TFT_ROTATION numbering */
	u8 reserved; /* retired `fps`: keeps the struct at 8 bytes, no padding */
	u8 decoder; /* DECODER_TYPE numbering */
};

struct pud_param_state {
	u32 settable; /* PUD_PARAM_* this build can change at runtime */
	u32 rejected; /* PUD_PARAM_* the last PUD_CMD_SET_PARAM could not apply */
	u8 brightness; /* values in effect now, read from the hardware/driver */
	u8 rotation;
	u8 reserved;
	u8 decoder;
};

/* Wire sizes of the parameter structs, checked where they are defined: the
 * `reserved` byte in each exists precisely to keep the layout aligned with no
 * hidden padding, and the host unpacks a fixed layout. */
#define PUD_PARAMS_SIZE 8
#define PUD_PARAM_STATE_SIZE 12
_Static_assert(sizeof(struct pud_params) == PUD_PARAMS_SIZE,
               "pud_params wire size");
_Static_assert(sizeof(struct pud_param_state) == PUD_PARAM_STATE_SIZE,
               "pud_param_state wire size");

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
 * so the end of a transfer never depends on a short packet.  A header that
 * declares more than frame_max - PUD_EP1_HEADER_SIZE, or a rectangle off the
 * panel, is not believable: the transfer is dropped and the endpoint re-armed,
 * counted by g_ep1_stat.oversize/bad.  It is deliberately *not* stalled -- see
 * notes/usb-protocol.md.  An odd `size` is accepted; a host may pad to even,
 * but does not have to (see the EP1 commentary in src/cherryusb/usb.c).
 */
#define PUD_EP1_HEADER_SIZE 12

struct pud_ep1_header {
	u16 xs;
	u16 ys;
	u16 xe;
	u16 ye;
	u32 size; /* payload bytes that follow */
};

/* DECODER_TYPE 6 keeps one dictionary window per frame slot, and which band a
 * window holds depends on how full the pipeline was when bands arrived: the
 * host cannot derive it from its own send count (measured: with a host slow
 * enough to keep one band in flight, every band lands in the same slot, and a
 * host that assumed round robin had 8 of 15 deltas refused).  So it asks.
 *
 * A DELTA names the band its dictionary was built from, and the device decodes
 * into whichever window holds that band, so this answer is what tells a host
 * which history it may still use. */
#define PUD_QOID_MAGIC 0x51445550u /* 'PUDQ' */
#define PUD_QOID_WINDOWS 4
struct pud_qoid_state {
	u32 magic;
	u32 slots; /* how many entries below are in use */
	u32 serial[PUD_QOID_WINDOWS]; /* band the window holds */
	u32 len[PUD_QOID_WINDOWS]; /* that band's QOI length */
	u32 valid[PUD_QOID_WINDOWS]; /* 1 when the window holds a band */
	/* PUD_DELTA_WIN: how big each window is.  The host needs it to know
	 * whether a band fits (a band's QOI over half of it is refused as
	 * oversize), and it is a build choice -- 32 KB on RP2350, 16 KB on
	 * RP2040 -- so a host that assumes one number breaks on the other. */
	u32 window;
};

/* Wire size, checked here like the other protocol structs: 9 u32 fields, no
 * padding.  The host pins the same layout as pud_usb.QOID_STATE. */
#define PUD_QOID_STATE_SIZE 60
_Static_assert(sizeof(struct pud_qoid_state) == PUD_QOID_STATE_SIZE,
               "pud_qoid_state wire size");

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

	struct disp_data disp;
	struct tp_data tp;
};

extern struct pud_data g_pud_data;

void pud_init(void);

/* Reads/writes below are clamped to the field, so an EP2 request can ask for
 * less than the whole field but never past it.  Only the ones a caller exists
 * for are declared; the EP2 replies read g_pud_data directly. */
void pud_get_ro_sn(u8 *ptr, int len);

void pud_get_ro_disp_xres(u16 *ptr, int len);
void pud_get_ro_disp_yres(u16 *ptr, int len);
void pud_get_ro_disp_pixelclock_khz(u16 *ptr, int len);

/* One touch poll: refresh g_pud_data.tp and push an EP4 report if there is
 * something to say.  Called from the indev task every polling_period ms. */
void pud_touch_poll(void);

#endif /* __PUD_H */
