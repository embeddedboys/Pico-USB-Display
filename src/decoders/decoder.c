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

#include <stdlib.h>
#include <string.h>

// #include "udd.h"
#include "tft.h"
#include "backlight.h"
#include "decoder.h"
#include "lz4.h"
#include "usb.h"
#include "bootlogo.h"

#include "pico/time.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

uint16_t decoder_xs, decoder_ys;
uint16_t decoder_xe, decoder_ye;

struct jpegdec_data {
	JPEGIMAGE img;
	u8 options;
};
struct jpegdec_data g_jpegdec;

int draw_mcus(JPEGDRAW *pDraw)
{
	/* iWidth is the MCU-pitch padded width; only iWidthUsed pixels are
	 * valid for edge/cropped blocks. Flushing the padded width smears
	 * garbage over the right edge of partial updates (skewed image).
	 */
	int iWidth = pDraw->iWidthUsed;
	int iCount = iWidth * pDraw->iHeight *
		     2; /* sizeof(*pDraw->pPixels) */
	int xs = pDraw->x;
	int ys = pDraw->y;
	int xe = pDraw->x + iWidth - 1;
	int ye = pDraw->y + pDraw->iHeight - 1;

	tft_video_flush(xs, ys, xe, ye, pDraw->pPixels, iCount);
	return iCount;
}

void jpegdec_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *jpeg_data,
		     u32 jpeg_size)
{
	static struct jpegdec_data *jpegdec = &g_jpegdec;
	int ret;

	JPEG_setPixelType(&jpegdec->img, RGB565_LITTLE_ENDIAN);

	ret = JPEG_openRAM(&jpegdec->img, jpeg_data, jpeg_size, draw_mcus);
	if (ret) {
		// pr_debug("Successfully opened JPEG image\n");
		// pr_debug("Image size: %d x %d, orientation: %d, bpp: %d\n",
		// 	JPEG_getWidth(&jpegdec->img),
		// 	JPEG_getHeight(&jpegdec->img),
		// 	JPEG_getOrientation(&jpegdec->img),
		// 	JPEG_getBpp(&jpegdec->img)
		// );
		JPEG_decode(&jpegdec->img, xs, ys, jpegdec->options);
	}
}

void lz4_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *lz4_data, u32 lz4_size)
{
	printf("%s, size :%d\n", __func__, lz4_size);
	char *lz4_workspace;
	int max_compressed_size =
		LZ4_compressBound(TFT_HOR_RES * TFT_VER_RES * 2);
	printf("%s, lz4 compreess boud: %d\n", __func__, max_compressed_size);

	lz4_workspace = (char *)malloc(max_compressed_size);
	if (lz4_workspace == NULL) {
		printf("%s, malloc workspace failed!", __func__);
		return;
	}

	int decompressed_size =
		LZ4_decompress_safe((char *)lz4_data, (char *)lz4_workspace,
				    lz4_size, max_compressed_size);
	printf("%s, decompressed_size: %d\n", __func__, decompressed_size);
	if (decompressed_size < 0)
		goto decompress_failed;

	tft_video_flush(xs, ys, xe, ye, lz4_workspace, decompressed_size);
decompress_failed:
	free(lz4_workspace);
}

/*
 * QOI (Quite OK Image, RGB565 variant) decoding.
 *
 * The stream is decoded in small batches via a callback so we never need to
 * hold a full frame in RAM: each decoded batch is flushed straight to the
 * TFT. The batch rectangle is relative to the frame; we add the frame origin.
 */
#include "rgb565_qoi.h"

#define QOI_BUF_ROWS 8

/* 480 x QOI_BUF_ROWS pixels per accumulation buffer, two ping-pong buffers */
static uint16_t qoi_buf_a[480 * QOI_BUF_ROWS];
static uint16_t qoi_buf_b[480 * QOI_BUF_ROWS];

struct qoi_draw_ctx {
	uint16_t ox;
	uint16_t oy;
};

/* Optional decode/display timing counters (see DECODER_STATS in the top level
 * CMakeLists.txt). Off by default; read them with gdb, e.g.
 *
 *   printf "%u %u %u %u %u\n", g_qoi_stat_draw_us, g_qoi_stat_flush_us, ...
 *
 * Within one gdb session they only ever increase, so measure a delta around a
 * known workload. */
#if DECODER_STATS
volatile u32 g_qoi_stat_draw_us;   /* whole qoi_drawimg() */
volatile u32 g_qoi_stat_flush_us;  /* time inside tft_video_flush() */
volatile u32 g_qoi_stat_pixels;    /* pixels flushed */
volatile u32 g_qoi_stat_calls;     /* qoi_flush() calls */
volatile u32 g_qoi_stat_frames;    /* qoi_drawimg() calls */

#define STAT_T0()          u32 _stat_t0 = time_us_32()
#define STAT_DRAW_ADD()    do { g_qoi_stat_draw_us += time_us_32() - _stat_t0; \
				g_qoi_stat_frames++; } while (0)
#define STAT_FLUSH_T0()    u32 _stat_ft0 = time_us_32()
#define STAT_FLUSH_ADD(n)  do { \
		g_qoi_stat_flush_us += time_us_32() - _stat_ft0; \
		g_qoi_stat_pixels += (n); g_qoi_stat_calls++; } while (0)
#else
#define STAT_T0()          do { } while (0)
#define STAT_DRAW_ADD()    do { } while (0)
#define STAT_FLUSH_T0()    do { } while (0)
#define STAT_FLUSH_ADD(n)  do { } while (0)
#endif

static void qoi_flush(const uint16_t *pixels, size_t count,
		      uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye,
		      void *user_data)
{
	struct qoi_draw_ctx *ctx = (struct qoi_draw_ctx *)user_data;

	STAT_FLUSH_T0();
	/*
	 * Asynchronous flush: this returns while the panel is still receiving the
	 * batch, so the decoder can start on the other ping-pong buffer. The
	 * transfer is completed by the next flush (or by the wait at the end of
	 * qoi_drawimg), which is what keeps the buffer reuse safe.
	 */
	tft_async_video_flush(ctx->ox + xs, ctx->oy + ys,
			      ctx->ox + xe, ctx->oy + ye,
			      (void *)pixels, count * 2);
	STAT_FLUSH_ADD(count);
}

void qoi_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *qoi_data, u32 qoi_size)
{
	struct qoi_draw_ctx ctx;
	uint16_t width = xe - xs + 1;
	size_t buf_cap;

	if (qoi_data == NULL || qoi_size == 0 || width == 0 || width > 480)
		return;

	ctx.ox = xs;
	ctx.oy = ys;

	/* Keep batches aligned to whole rows: a multiple of the frame width,
	 * capped at the static buffer size. */
	buf_cap = (size_t)width * QOI_BUF_ROWS;
	if (buf_cap > 480 * QOI_BUF_ROWS)
		buf_cap = 480 * QOI_BUF_ROWS;

	STAT_T0();
	rgb565_qoi_decompress_callback(qoi_data, qoi_size, width,
				       qoi_buf_a, qoi_buf_b,
				       buf_cap, qoi_flush, &ctx);
	/* the last batch is still in flight; the frame is not done until it lands */
	tft_async_video_wait();
	STAT_DRAW_ADD();
}

void decoder_set_window(u16 xs, u16 ys, u16 xe, u16 ye)
{
	/* Called from the USB ISR only, so no blocking is allowed. The decoder
	 * task reads the window from the submitted frame instead of these
	 * globals.
	 */
	decoder_xs = xs;
	decoder_ys = ys;
	decoder_xe = xe;
	decoder_ye = ye;
}

/*
 * JPEG decoding and the TFT flush must NOT run on the USB interrupt stack:
 * the decode path needs a large stack and holding the USB IRQ for tens of
 * milliseconds wedges the USB controller and corrupts the interrupt stack.
 * The USB ISR therefore just copies the received frame into a slot and wakes
 * a dedicated decoder task which does the actual work.
 */
#define DECODER_FRAME_SLOTS 2
/* One slot has to hold any transfer the control stage accepts, so it is sized
 * by the same per-board constant as ep1_read_buffer (PUD_MAX_TRANSFER, see
 * usbd_vendor.h). */
#define DECODER_FRAME_MAX   PUD_MAX_TRANSFER

_Static_assert(DECODER_FRAME_MAX >= PUD_MAX_TRANSFER,
	       "a frame slot must be able to hold a whole EP1 transfer");

struct decoder_frame {
	u16 xs, ys, xe, ye;
	u32 size;
	u8 busy;
	u8 data[DECODER_FRAME_MAX];
};

static struct decoder_frame s_frames[DECODER_FRAME_SLOTS];
static SemaphoreHandle_t s_decoder_sem;

/* Diagnostics, readable from a debugger: frames seen / dropped / drawn.
 * A non-zero drop count means the host is outrunning the decoder and some
 * partial updates never reach the panel (visible as stale regions).
 * `oversize` counts transfers that did not fit a frame slot: the control
 * stage (see usbd_vendor_ep1_size_ok) is supposed to refuse those first, so a
 * non-zero value means device and host disagree about the frame size. */
volatile u32 g_decoder_stat_submitted;
volatile u32 g_decoder_stat_dropped;
volatile u32 g_decoder_stat_drawn;
volatile u32 g_decoder_stat_oversize;

/* True when at least one frame slot is idle, i.e. the USB stack may arm
 * EP1 for the next frame.  Used for EP1 flow control (see usb.h). */
bool decoder_slot_free(void)
{
	int i;

	for (i = 0; i < DECODER_FRAME_SLOTS; i++) {
		if (!s_frames[i].busy)
			return true;
	}

	return false;
}

void decoder_submit_frame(u16 xs, u16 ys, u16 xe, u16 ye, const u8 *data,
			  u32 size)
{
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	int i;

	/* Drop rather than truncate.  The old code clamped the size and decoded
	 * a truncated stream, which corrupted the image with no error visible
	 * anywhere.  The control stage refuses oversized transfers first, so
	 * this is a last line of defence. */
	if (size > DECODER_FRAME_MAX) {
		g_decoder_stat_oversize++;
		return;
	}

	g_decoder_stat_submitted++;

	for (i = 0; i < DECODER_FRAME_SLOTS; i++) {
		if (!s_frames[i].busy) {
			s_frames[i].xs = xs;
			s_frames[i].ys = ys;
			s_frames[i].xe = xe;
			s_frames[i].ye = ye;
			s_frames[i].size = size;
			memcpy(s_frames[i].data, data, size);
			s_frames[i].busy = 1;
			xSemaphoreGiveFromISR(s_decoder_sem,
					      &xHigherPriorityTaskWoken);
			break;
		}
	}

	if (i == DECODER_FRAME_SLOTS)
		g_decoder_stat_dropped++;

	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static void decoder_task(void *param)
{
	int i;

	(void)param;

	/* First paint: the boot logo, before any host frame.  The panel has
	 * exactly one writer (this task), so no lock is needed, and doing it
	 * here instead of in a task of its own both drops a task and makes
	 * "the logo is the first thing drawn" a hard guarantee.  The USB side
	 * is independent: enumeration runs on core 0 while this draws. */
	decoder_drawimg(0, 0, TFT_HOR_RES - 1, TFT_VER_RES - 1,
			(uint8_t *)bootlogo, sizeof(bootlogo));

	busy_wait_ms(10);
	backlight_set_level(100);
	printf("backlight set to 100%%\n");

	for (;;) {
		xSemaphoreTake(s_decoder_sem, portMAX_DELAY);

		for (i = 0; i < DECODER_FRAME_SLOTS; i++) {
			if (s_frames[i].busy) {
				decoder_drawimg(s_frames[i].xs, s_frames[i].ys,
						s_frames[i].xe, s_frames[i].ye,
						s_frames[i].data,
						s_frames[i].size);
				s_frames[i].busy = 0;
				g_decoder_stat_drawn++;
				/* A slot is free again: re-arm EP1 if the
				 * host's request was deferred. */
				usbd_vendor_ep1_tick();
				break;
			}
		}
	}
}

/* Indexed by DECODER_TYPE.  Slot 0 is unused -- tjpgd was never implemented --
 * but it is kept so the other numbers do not shift: the device reports this
 * value to the host through PUD_CMD_GET_CAPS. */
static char *decoder_names[] = { "(unused)", "JPEGDEC", "LZ4", "QOI" };

void decoder_init(void)
{
	s_decoder_sem = xSemaphoreCreateBinary();
	/* 1024 words = 4 KB.  Measured peak of the whole decode path (0xa5 fill
	 * scan, see notes/debugging.md): JPEGDEC 632 B for a full 480x320 image,
	 * QOI 496 B.  The old 4096 words was sized on the assumption that
	 * JPEGDEC eats stack, which the measurement does not support: JPEGDEC
	 * keeps its context in .bss (&g_jpegdec) and its MCU callback only uses
	 * scalars.  4 KB keeps ~6x margin over the worst case, which matters on
	 * RP2040 (264 KB SRAM, and no PSPLIM stack guard on Cortex-M0+). */
	xTaskCreate(decoder_task, "decoder_task", 1024, NULL,
		    tskIDLE_PRIORITY + 1, NULL);
	printf("Decoder type: %s\n", decoder_names[DECODER_TYPE]);
}
