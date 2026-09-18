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

/*
 * tjpgd (ChaN's TJpgDec R0.03, vendored in src/decoders/tjpgd/).
 *
 * tjpgdcnf.h sets JD_FORMAT 1, so the output callback receives RGB565 in the
 * same byte order the panel takes, and JD_FASTDECODE 2, which makes the
 * workspace TJPGD_WORKSPACE_SIZE (9.4 KB).  Both are static: the decoder
 * task's stack is only sized for call frames.
 *
 * tjpgd hands out one 8x8 block at a time, so flushing each block straight to
 * the panel would cost one address-window setup per block -- 2400 of them for
 * a 480x320 image.  Collect whole rows of blocks and flush one rectangle per
 * group instead, for the same reason the QOI path batches its output.
 *
 * Unlike QOI this decodes a whole image per call, so it is only useful for
 * whole-rectangle updates (the host sends a self-contained JPEG of the
 * rectangle, exactly like the JPEGDEC path).
 */
#include "tjpgd.h"

static uint8_t s_tjpgd_workspace[TJPGD_WORKSPACE_SIZE] __attribute__((aligned(4)));

/* Rows of blocks buffered before one flush; 8 rows x 480 px x 2 B = 7.5 KB.
 * The buffer is always indexed with the visible width as the row pitch, so a
 * flush is a contiguous run even for a narrower image. */
#define TJPGD_GROUP_ROWS 8
static u16 s_tjpgd_rowbuf[TFT_HOR_RES * TJPGD_GROUP_ROWS];

struct tjpgd_ctx {
	const u8 *data;
	u32 size;
	u32 index;
	int16_t x, y;	/* where the image goes on the panel */
	u16 width;	/* visible width of the image (row pitch of the buffer) */
	u16 group;	/* row group currently buffered */
	u16 group_rows;	/* how many rows of it are filled */
	bool group_valid;
};

static struct tjpgd_ctx s_tjpgd;

static size_t tjpgd_input(JDEC *jdec, uint8_t *buf, size_t len)
{
	(void)jdec;

	if (s_tjpgd.index + len > s_tjpgd.size)
		len = s_tjpgd.size - s_tjpgd.index;

	memcpy(buf, s_tjpgd.data + s_tjpgd.index, len);
	s_tjpgd.index += len;

	return len;
}

static void tjpgd_flush_group(void)
{
	int y = s_tjpgd.y + s_tjpgd.group * TJPGD_GROUP_ROWS;
	u16 rows = s_tjpgd.group_rows;

	if (!s_tjpgd.group_valid)
		return;
	s_tjpgd.group_valid = false;
	s_tjpgd.group_rows = 0;

	if (rows == 0 || y >= TFT_VER_RES || s_tjpgd.width == 0)
		return;
	if (y + rows > TFT_VER_RES)
		rows = TFT_VER_RES - y;

	tft_video_flush(s_tjpgd.x, y, s_tjpgd.x + s_tjpgd.width - 1,
			y + rows - 1, s_tjpgd_rowbuf,
			s_tjpgd.width * rows * 2);
}

static int tjpgd_output(JDEC *jdec, void *bitmap, JRECT *rect)
{
	/* Clip the padding tjpgd adds to the last MCU row and column before it
	 * reaches the panel -- the trap JPEGDEC's iWidthUsed exists for. */
	u16 pitch = rect->right - rect->left + 1;
	u16 bh = rect->bottom - rect->top + 1;
	u16 bw = pitch;
	u16 *px = (u16 *)bitmap;
	u16 r;

	if (rect->left + bw > jdec->width)
		bw = jdec->width - rect->left;
	if (rect->top + bh > jdec->height)
		bh = jdec->height - rect->top;
	if (bw > s_tjpgd.width)
		bw = s_tjpgd.width;
	if (bw == 0 || bh == 0)
		return 1;

	/* One call can cover a whole MCU, and a 4:2:0 MCU is 16 rows tall while
	 * the buffer holds TJPGD_GROUP_ROWS: walk the block row by row and flush
	 * every time it would leave the buffered group, so the buffer size does
	 * not depend on the JPEG's chroma subsampling. */
	for (r = 0; r < bh; r++) {
		u16 row = rect->top + r;
		u16 group = row / TJPGD_GROUP_ROWS;
		u16 row0 = row - group * TJPGD_GROUP_ROWS;

		if (s_tjpgd.group_valid && group != s_tjpgd.group)
			tjpgd_flush_group();

		s_tjpgd.group = group;
		s_tjpgd.group_valid = true;

		memcpy(&s_tjpgd_rowbuf[row0 * s_tjpgd.width + rect->left],
		       px + r * pitch, bw * 2);

		if (row0 + 1 > s_tjpgd.group_rows)
			s_tjpgd.group_rows = row0 + 1;
	}

	return 1;	/* continue decoding */
}

void tjpgd_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *jpeg_data, u32 jpeg_size)
{
	JDEC jdec;
	JRESULT res;

	/* The image carries its own size; xe/ye only repeat it. */
	(void)xe;
	(void)ye;

	s_tjpgd.data = jpeg_data;
	s_tjpgd.size = jpeg_size;
	s_tjpgd.index = 0;
	s_tjpgd.x = xs;
	s_tjpgd.y = ys;
	s_tjpgd.group = 0;
	s_tjpgd.group_rows = 0;
	s_tjpgd.group_valid = false;

	memset(&jdec, 0, sizeof(jdec));
	jdec.swap = false;	/* JD_FORMAT 1 already gives panel byte order */

	res = jd_prepare(&jdec, tjpgd_input, s_tjpgd_workspace,
			 sizeof(s_tjpgd_workspace), NULL);
	if (res != JDR_OK) {
		printf("tjpgd: jd_prepare failed: %d\n", res);
		return;
	}

	/* Only the part that lands on the panel is worth buffering. */
	s_tjpgd.width = jdec.width;
	if (s_tjpgd.x + s_tjpgd.width > TFT_HOR_RES)
		s_tjpgd.width = TFT_HOR_RES - s_tjpgd.x;
	if (s_tjpgd.width > TFT_HOR_RES)
		s_tjpgd.width = TFT_HOR_RES;

	res = jd_decomp(&jdec, tjpgd_output, 0);
	tjpgd_flush_group();
	if (res != JDR_OK)
		printf("tjpgd: jd_decomp failed: %d\n", res);
}

/*
 * Batch ping-pong for the callback decoders (QOI, RLE).
 *
 * Their libraries hand us one batch at a time and let us choose between two
 * accumulation buffers: with a second buffer the next batch can be decoded
 * while the previous one is still being written to the panel (the flush is
 * asynchronous), with one buffer every batch has to land before the next one
 * starts.  Build with -DPUD_DECODER_PINGPONG=0 to measure what the overlap is
 * worth on a given workload -- it also drops one 7680 byte buffer per decoder.
 *
 * Correctness does not depend on the choice: single buffer mode uses the
 * synchronous flush, so the decoder never touches a buffer that is in flight.
 */
#ifndef PUD_DECODER_PINGPONG
#define PUD_DECODER_PINGPONG 1
#endif

/*
 * Optional decode/display timing counters, shared by every decoder path
 * (see DECODER_STATS in the top level CMakeLists.txt).  Off by default; read
 * them with gdb, e.g.
 *
 *   printf "%u %u %u %u %u\n", g_qoi_stat_draw_us, g_qoi_stat_flush_us, ...
 *
 * Within one gdb session they only ever increase, so measure a delta around a
 * known workload.
 */
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

/*
 * LZ4 (block format, i.e. what LZ4_compress_default() produces -- the same
 * function the kernel has built in, which is why this codec is here at all).
 *
 * An LZ4 block cannot be decoded in pieces: every match points back at output
 * the same block produced earlier, so the whole block has to land in one
 * contiguous buffer, and that buffer doubles as the dictionary.  A full
 * 480x320 frame is 300 KB, which is why the old code's
 * malloc(LZ4_compressBound(frame)) could never succeed here.
 *
 * The device therefore holds one *band*, not one frame. The host splits the
 * damage rectangle the same way it already does for QOI and RLE -- by
 * `band_pixels` from PUD_CMD_GET_CAPS -- and every transfer carries a
 * self-contained block for that rectangle. Nothing about the protocol changes:
 * a band is just the rectangle of this transfer, as for the other codecs.
 *
 * The buffer is sized by that same rule plus one pixel, because the device
 * rounds ((PUD_MAX_TRANSFER - 16) / 3) where the host rounds
 * ((min(65535, frame_max) - 16) / 3), so any band the host may send fits.
 */
#define LZ4_BAND_PIXELS (((PUD_MAX_TRANSFER - 16) / 3) + 1)
static uint16_t lz4_band[LZ4_BAND_PIXELS] __attribute__((aligned(4)));

/* Readable from a debugger, like the frame counters below.  A non-zero value
 * means the host sent something this device cannot use: `oversize` is a band
 * bigger than lz4_band[] (the host banded too coarsely), `bad` is a block that
 * did not decode to exactly the window's worth of pixels (truncated, corrupt,
 * or encoded for a different rectangle).  Both drop the band instead of
 * drawing garbage. */
volatile u32 g_decoder_stat_lz4_oversize;
volatile u32 g_decoder_stat_lz4_bad;

void lz4_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *lz4_data, u32 lz4_size)
{
	u32 width = (u32)(xe - xs + 1);
	u32 height = (u32)(ye - ys + 1);
	u32 raw = width * height * 2u;
	int got;

	if (lz4_data == NULL || lz4_size == 0 || width == 0 || height == 0)
		return;

	if (raw > sizeof(lz4_band)) {
		g_decoder_stat_lz4_oversize++;
		return;
	}

	STAT_T0();
	got = LZ4_decompress_safe((const char *)lz4_data, (char *)lz4_band,
				  (int)lz4_size, (int)raw);
	if (got != (int)raw) {
		g_decoder_stat_lz4_bad++;
		return;
	}

	STAT_FLUSH_T0();
	/*
	 * Asynchronous, then wait: a decoded band is only ever written once, so
	 * unlike the QOI/RLE batches it does not need a ping-pong pair, but it
	 * does have to be finished before the next band is decoded into the same
	 * buffer.
	 */
	tft_async_video_flush(xs, ys, xe, ye, lz4_band, raw);
	STAT_FLUSH_ADD(raw / 2u);
	tft_async_video_wait();
	STAT_DRAW_ADD();
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
#if PUD_DECODER_PINGPONG
static uint16_t qoi_buf_b[480 * QOI_BUF_ROWS];
#endif

struct qoi_draw_ctx {
	uint16_t ox;
	uint16_t oy;
};

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
#if PUD_DECODER_PINGPONG
	tft_async_video_flush(ctx->ox + xs, ctx->oy + ys,
			      ctx->ox + xe, ctx->oy + ye,
			      (void *)pixels, count * 2);
#else
	/* one buffer: it has to be free before the decoder fills it again */
	tft_video_flush(ctx->ox + xs, ctx->oy + ys,
			ctx->ox + xe, ctx->oy + ye,
			(void *)pixels, count * 2);
#endif
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
				       qoi_buf_a,
#if PUD_DECODER_PINGPONG
				       qoi_buf_b,
#else
				       NULL,
#endif
				       buf_cap, qoi_flush, &ctx);
	/* the last batch is still in flight; the frame is not done until it lands */
	tft_async_video_wait();
	STAT_DRAW_ADD();
}

/*
 * RGB565 RLE (vendored in src/decoders/rle/).
 *
 * Same shape as the QOI path: the library decodes into two ping-pong buffers
 * and calls back once per batch, so a batch can be pushed to the panel while
 * the next one is decoded.  It keeps no state of its own, so unlike the JPEG
 * decoders there is no workspace to account for.
 */
#include "rgb565_rle.h"

#define RLE_BUF_ROWS 8

/* 480 x RLE_BUF_ROWS pixels per accumulation buffer, two ping-pong buffers */
static uint16_t rle_buf_a[480 * RLE_BUF_ROWS];
#if PUD_DECODER_PINGPONG
static uint16_t rle_buf_b[480 * RLE_BUF_ROWS];
#endif

struct rle_draw_ctx {
	uint16_t ox;
	uint16_t oy;
};

static void rle_flush(const uint16_t *pixels, size_t count,
		      uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye,
		      void *user_data)
{
	struct rle_draw_ctx *ctx = (struct rle_draw_ctx *)user_data;

	STAT_FLUSH_T0();
	/* asynchronous, same contract as the QOI flush: the buffer may only be
	 * reused once the next flush (or the wait below) has completed it. */
#if PUD_DECODER_PINGPONG
	tft_async_video_flush(ctx->ox + xs, ctx->oy + ys,
			      ctx->ox + xe, ctx->oy + ye,
			      (void *)pixels, count * 2);
#else
	/* one buffer: it has to be free before the decoder fills it again */
	tft_video_flush(ctx->ox + xs, ctx->oy + ys,
			ctx->ox + xe, ctx->oy + ye,
			(void *)pixels, count * 2);
#endif
	STAT_FLUSH_ADD(count);
}

void rle_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *rle_data, u32 rle_size)
{
	struct rle_draw_ctx ctx;
	uint16_t width = xe - xs + 1;
	size_t buf_cap;

	if (rle_data == NULL || rle_size == 0 || width == 0 || width > 480)
		return;

	ctx.ox = xs;
	ctx.oy = ys;

	/* whole rows per batch, capped at the static buffers */
	buf_cap = (size_t)width * RLE_BUF_ROWS;
	if (buf_cap > 480 * RLE_BUF_ROWS)
		buf_cap = 480 * RLE_BUF_ROWS;

	STAT_T0();
	rgb565_rle_decompress_callback(rle_data, rle_size, width,
				       rle_buf_a,
#if PUD_DECODER_PINGPONG
				       rle_buf_b,
#else
				       NULL,
#endif
				       buf_cap, rle_flush, &ctx);
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

/*
 * Draw the boot logo.
 *
 * QOI, RLE and both JPEG decoders store one stream for the whole panel, so the
 * logo is just the first frame.  LZ4 cannot: a single block would have to be
 * decoded in one piece (see lz4_drawimg()), so its logo is stored the way the
 * device consumes it -- the same [count][offsets][data...] container the
 * conversion tool writes for a frame sequence, one block per band, and the
 * bands are uniform so the row height follows from the count.  Regenerating it
 * is `pudcodec video2s --raw <raw565> -w 480 -h <rows> --codec lz4 -t bin`;
 * see notes/scripts.md.
 */
static void decoder_draw_bootlogo(void)
{
#if DECODER_TYPE == DECODER_USE_LZ4
	const u8 *p = (const u8 *)bootlogo;
	u32 count = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
		   ((u32)p[3] << 24);
	u32 rows;
	u32 i;

	if (count == 0 || count > TFT_VER_RES || TFT_VER_RES % count != 0)
		return;   /* not a container: nothing sane to draw */
	rows = TFT_VER_RES / count;

	for (i = 0; i < count; i++) {
		const u8 *e = p + 4u + 4u * (i + 1u);
		u32 start = (u32)p[4u + 4u * i] |
			    ((u32)p[4u + 4u * i + 1u] << 8) |
			    ((u32)p[4u + 4u * i + 2u] << 16) |
			    ((u32)p[4u + 4u * i + 3u] << 24);
		u32 end = (u32)e[0] | ((u32)e[1] << 8) | ((u32)e[2] << 16) |
			  ((u32)e[3] << 24);
		u32 y = i * rows;

		if (end <= start || end > sizeof(bootlogo))
			break;
		lz4_drawimg(0, (u16)y, TFT_HOR_RES - 1,
			    (u16)(i + 1u == count ? TFT_VER_RES - 1
						  : y + rows - 1),
			    (u8 *)p + start, end - start);
	}
#else
	decoder_drawimg(0, 0, TFT_HOR_RES - 1, TFT_VER_RES - 1,
			(uint8_t *)bootlogo, sizeof(bootlogo));
#endif
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
	decoder_draw_bootlogo();

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

/* Indexed by DECODER_TYPE.  The numbering must not shift: the device reports
 * this value to the host through PUD_CMD_GET_CAPS. */
static char *decoder_names[] = { "tjpgd", "JPEGDEC", "LZ4", "QOI", "RLE" };

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
