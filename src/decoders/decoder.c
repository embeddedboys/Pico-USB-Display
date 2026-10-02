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
#include "pud.h" /* PUD_EP1_HEADER_SIZE, the EP1 framing */
#if DECODER_TYPE == DECODER_USE_QOIZ
/* QOI+deflate has no logo branch of its own: the logo is plain QOI, drawn with
 * qoi_drawimg() (see decoder_draw_bootlogo), so take the QOI array while
 * bootlogo.h is being included.  Each type restates its own restore rather than
 * stashing the old value in another macro: a macro body is expanded where it is
 * used, so a stashed copy would come back as the token DECODER_TYPE again. */
#undef DECODER_TYPE
#define DECODER_TYPE DECODER_USE_QOI
#include "bootlogo.h"
#undef DECODER_TYPE
#define DECODER_TYPE DECODER_USE_QOIZ
#elif DECODER_TYPE == DECODER_USE_QOID
/* same trick: the dictionary variant decodes QOI underneath too */
#undef DECODER_TYPE
#define DECODER_TYPE DECODER_USE_QOI
#include "bootlogo.h"
#undef DECODER_TYPE
#define DECODER_TYPE DECODER_USE_QOID
#else
#include "bootlogo.h"
#endif

#include "pico/time.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

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
	int iCount = iWidth * pDraw->iHeight * 2; /* sizeof(*pDraw->pPixels) */
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

static uint8_t s_tjpgd_workspace[TJPGD_WORKSPACE_SIZE]
        __attribute__((aligned(4)));

/* Rows of blocks buffered before one flush; 8 rows x 480 px x 2 B = 7.5 KB.
 * The buffer is always indexed with the visible width as the row pitch, so a
 * flush is a contiguous run even for a narrower image. */
#define TJPGD_GROUP_ROWS 8
/* Sized by the larger of the two, because a 90 degree rotation asked for at
 * runtime swaps width and height: max() of the build pair is the same either way. */
static u16 s_tjpgd_rowbuf[MAX(TFT_HOR_RES, TFT_VER_RES) * TJPGD_GROUP_ROWS];

struct tjpgd_ctx {
	const u8 *data;
	u32 size;
	u32 index;
	int16_t x, y; /* where the image goes on the panel */
	u16 width; /* visible width of the image (row pitch of the buffer) */
	u16 group; /* row group currently buffered */
	u16 group_rows; /* how many rows of it are filled */
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

	if (rows == 0 || y >= g_pud_data.disp.yres || s_tjpgd.width == 0)
		return;
	if (y + rows > g_pud_data.disp.yres)
		rows = g_pud_data.disp.yres - y;

	tft_video_flush(s_tjpgd.x, y, s_tjpgd.x + s_tjpgd.width - 1,
	                y + rows - 1, s_tjpgd_rowbuf, s_tjpgd.width * rows * 2);
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

	return 1; /* continue decoding */
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
	jdec.swap = false; /* JD_FORMAT 1 already gives panel byte order */

	res = jd_prepare(&jdec, tjpgd_input, s_tjpgd_workspace,
	                 sizeof(s_tjpgd_workspace), NULL);
	if (res != JDR_OK) {
		printf("tjpgd: jd_prepare failed: %d\n", res);
		return;
	}

	/* Only the part that lands on the panel is worth buffering. */
	s_tjpgd.width = jdec.width;
	if (s_tjpgd.x + s_tjpgd.width > g_pud_data.disp.xres)
		s_tjpgd.width = g_pud_data.disp.xres - s_tjpgd.x;
	if (s_tjpgd.width > g_pud_data.disp.xres)
		s_tjpgd.width = g_pud_data.disp.xres;

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
volatile u32 g_qoi_stat_draw_us; /* whole qoi_drawimg() */
volatile u32 g_qoi_stat_flush_us; /* time inside tft_video_flush() */
volatile u32 g_qoi_stat_pixels; /* pixels flushed */
volatile u32 g_qoi_stat_calls; /* qoi_flush() calls */
volatile u32 g_qoi_stat_frames; /* qoi_drawimg() calls */

#define STAT_T0() u32 _stat_t0 = time_us_32()
#define STAT_DRAW_ADD()                                        \
	do {                                                   \
		g_qoi_stat_draw_us += time_us_32() - _stat_t0; \
		g_qoi_stat_frames++;                           \
	} while (0)
#define STAT_FLUSH_T0() u32 _stat_ft0 = time_us_32()
#define STAT_FLUSH_ADD(n)                                        \
	do {                                                     \
		g_qoi_stat_flush_us += time_us_32() - _stat_ft0; \
		g_qoi_stat_pixels += (n);                        \
		g_qoi_stat_calls++;                              \
	} while (0)
#else
#define STAT_T0() \
	do {      \
	} while (0)
#define STAT_DRAW_ADD() \
	do {            \
	} while (0)
#define STAT_FLUSH_T0() \
	do {            \
	} while (0)
#define STAT_FLUSH_ADD(n) \
	do {              \
	} while (0)
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
 * The buffer is sized by that same rule plus one pixel: the device rounds
 * ((PUD_MAX_TRANSFER - PUD_EP1_HEADER_SIZE - 16) / 3) where the host rounds
 * ((min(65535, frame_max) - PUD_EP1_HEADER_SIZE - 16) / 3), so any band the
 * host may send fits.  The 16 bytes are the codec's own framing (QOI's header
 * plus end marker), the 12 are the EP1 header in front of every payload.
 */
#define LZ4_BAND_PIXELS \
	(((PUD_MAX_TRANSFER - PUD_EP1_HEADER_SIZE - 16) / 3) + 1)
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

#ifndef QOI_BUF_ROWS
#define QOI_BUF_ROWS 8
#endif
/* 0 = callback API only, 1 = non-callback without overlap (measurement only),
 * 2 = non-callback with band ping-pong (default; see qoi_drawimg). */
#ifndef QOI_NONCALLBACK
#define QOI_NONCALLBACK 2
#endif

/* 480 x QOI_BUF_ROWS pixels per accumulation buffer, two ping-pong buffers */
static uint16_t qoi_buf_a[480 * QOI_BUF_ROWS];
#if PUD_DECODER_PINGPONG
static uint16_t qoi_buf_b[480 * QOI_BUF_ROWS];
#endif

#if QOI_NONCALLBACK >= 2
/* Two whole-band buffers: one is being written to the panel while the next
 * band is decoded into the other (see qoi_drawimg). */
static uint16_t qoi_band[2 * LZ4_BAND_PIXELS] __attribute__((aligned(4)));
static unsigned qoi_band_next;
#elif QOI_NONCALLBACK
/* One whole-band buffer, without the overlap (for A/B measurements). */
static uint16_t qoi_band[LZ4_BAND_PIXELS] __attribute__((aligned(4)));
#endif

struct qoi_draw_ctx {
	uint16_t ox;
	uint16_t oy;
};

static void qoi_flush(const uint16_t *pixels, size_t count, uint16_t xs,
                      uint16_t ys, uint16_t xe, uint16_t ye, void *user_data)
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
	tft_async_video_flush(ctx->ox + xs, ctx->oy + ys, ctx->ox + xe,
	                      ctx->oy + ye, (void *)pixels, count * 2);
#else
	/* one buffer: it has to be free before the decoder fills it again */
	tft_video_flush(ctx->ox + xs, ctx->oy + ys, ctx->ox + xe, ctx->oy + ye,
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

#if QOI_NONCALLBACK
	/*
	 * Non-callback path: decode the whole transfer as one image into one
	 * band-sized buffer and flush it in a single window.  This is possible
	 * because the host bands every transfer by band_pixels, so one transfer
	 * is one rectangle that fits this buffer -- the same argument LZ4 relies
	 * on.  It is a lot faster than the streaming callback API (that one pays
	 * a per-pixel accumulate/capacity/callback check), and with two buffers
	 * the panel write of one band overlaps the decode of the next.
	 */
	{
		size_t rect_px = (size_t)width * (size_t)(ye - ys + 1);

		if (rect_px <= LZ4_BAND_PIXELS) {
#if QOI_NONCALLBACK >= 2
			uint16_t *buf =
			        qoi_band + qoi_band_next * LZ4_BAND_PIXELS;
#else
			uint16_t *buf = qoi_band;
#endif

			STAT_T0();
			if (rgb565_qoi_decompress(qoi_data, qoi_size, buf,
			                          rect_px) == rect_px) {
				STAT_FLUSH_T0();
				tft_async_video_flush(xs, ys, xe, ye, buf,
				                      rect_px * 2);
				STAT_FLUSH_ADD(rect_px);
#if QOI_NONCALLBACK >= 2
				/* The next flush's window command waits for this
				 * transfer (i80_finish_pending), by which time the
				 * next band has been decoded into the other buffer,
				 * so decode and panel write overlap. */
				qoi_band_next ^= 1u;
#else
				tft_async_video_wait();
#endif
			}
			STAT_DRAW_ADD();
			return;
		}
	}
#endif

	/* Keep batches aligned to whole rows: a multiple of the frame width,
	 * capped at the static buffer size. */
	buf_cap = (size_t)width * QOI_BUF_ROWS;
	if (buf_cap > 480 * QOI_BUF_ROWS)
		buf_cap = 480 * QOI_BUF_ROWS;

	STAT_T0();
	rgb565_qoi_decompress_callback(qoi_data, qoi_size, width, qoi_buf_a,
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

static void rle_flush(const uint16_t *pixels, size_t count, uint16_t xs,
                      uint16_t ys, uint16_t xe, uint16_t ye, void *user_data)
{
	struct rle_draw_ctx *ctx = (struct rle_draw_ctx *)user_data;

	STAT_FLUSH_T0();
	/* asynchronous, same contract as the QOI flush: the buffer may only be
	 * reused once the next flush (or the wait below) has completed it. */
#if PUD_DECODER_PINGPONG
	tft_async_video_flush(ctx->ox + xs, ctx->oy + ys, ctx->ox + xe,
	                      ctx->oy + ye, (void *)pixels, count * 2);
#else
	/* one buffer: it has to be free before the decoder fills it again */
	tft_video_flush(ctx->ox + xs, ctx->oy + ys, ctx->ox + xe, ctx->oy + ye,
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
	rgb565_rle_decompress_callback(rle_data, rle_size, width, rle_buf_a,
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

#if DECODER_TYPE == DECODER_USE_QOIZ
/*
 * QOI + deflate (experimental, DECODER_TYPE 5).
 *
 * The host QOI-encodes a band exactly as for DECODER_TYPE 3 and then runs raw
 * deflate (RFC 1951, no zlib header) over that stream: QOI leaves the repeats
 * *between* pixels on the table -- the same text glyph, the same row of a
 * gradient -- and an LZ77 stage picks them up (measured on the desktop regions:
 * 25~31% fewer bytes than QOI alone, see notes/decoders.md).
 *
 * The device inflates the transfer into one buffer and hands that to
 * qoi_drawimg(), so everything after the inflate -- the non-callback decode, the
 * band ping-pong, the panel write -- is the QOI path unchanged.  One buffer is
 * enough: the QOI decode is synchronous and finishes reading it before
 * qoi_drawimg() returns (the asynchronous flush reads qoi_band[], not this).
 *
 * The inflated stream is a QOI band, which is at most 3 bytes per pixel plus 16
 * bytes of framing for band_pixels pixels -- i.e. it fits a transfer, which is
 * how band_pixels was derived in the first place.  A stream that inflates to
 * more than that, or not at all, is dropped and counted, never truncated.
 */
static u8 qoiz_buf[PUD_MAX_TRANSFER] __attribute__((aligned(4)));

/*
 * The inflate is a build choice (PUD_INFLATE, see CMakeLists.txt): both decode
 * the same raw deflate stream, so the host cannot tell them apart.
 *   1  tinfl (miniz 3.0.2): a resumable state machine, 8.2 KB of state
 *   2  libdeflate v1.26: one-shot, wants the whole input and output at once --
 *      which is exactly how a transfer arrives
 */
#if PUD_INFLATE == 2
#include "libdeflate.h"

/*
 * libdeflate's decompressor struct is opaque and allocated through a hook, so
 * hand it one static block instead of the heap: the first (and only) request is
 * served from here, anything after that fails.  The size is checked against the
 * request rather than assumed.
 */
static u8 qoiz_ld_mem[12 * 1024] __attribute__((aligned(8)));
static bool qoiz_ld_used;
static struct libdeflate_decompressor *qoiz_ld;

static void *qoiz_ld_alloc(size_t size)
{
	if (qoiz_ld_used || size > sizeof(qoiz_ld_mem))
		return NULL;
	qoiz_ld_used = true;
	return qoiz_ld_mem;
}

static void qoiz_ld_free(void *p)
{
	(void)p;
}
#else
#include "miniz.h"

static tinfl_decompressor qoiz_inflator;
#endif

/* Diagnostics: transfers whose deflate stream was corrupt, and ones whose
 * output would not fit qoiz_buf. */
volatile u32 g_decoder_stat_qoiz_bad;
volatile u32 g_decoder_stat_qoiz_oversize;
#if DECODER_STATS
volatile u32 g_qoiz_stat_inflate_us; /* time spent inflating */
volatile u32 g_qoiz_stat_in_bytes; /* deflate bytes consumed */
volatile u32 g_qoiz_stat_out_bytes; /* QOI bytes produced */
#endif

/* Inflate one transfer into qoiz_buf.  Returns the QOI byte count, or 0 after
 * counting why it failed. */
static size_t qoiz_inflate(const u8 *in, size_t in_size)
{
#if PUD_INFLATE == 2
	size_t in_len = 0, out_len = 0;
	enum libdeflate_result res;

	if (qoiz_ld == NULL) {
		struct libdeflate_options opts = {
			.sizeof_options = sizeof(opts),
			.malloc_func = qoiz_ld_alloc,
			.free_func = qoiz_ld_free,
		};

		qoiz_ld = libdeflate_alloc_decompressor_ex(&opts);
		if (qoiz_ld == NULL) {
			/* qoiz_ld_mem is smaller than this libdeflate needs */
			g_decoder_stat_qoiz_bad++;
			return 0;
		}
	}

	/* Decoding stops at the final block, so a pad byte after it is fine;
	 * actual_out_nbytes_ret is given because the QOI length is not known
	 * up front (only its upper bound, the buffer). */
	res = libdeflate_deflate_decompress_ex(qoiz_ld, in, in_size, qoiz_buf,
	                                       sizeof(qoiz_buf), &in_len,
	                                       &out_len);
#if DECODER_STATS
	g_qoiz_stat_in_bytes += in_len;
	g_qoiz_stat_out_bytes += out_len;
#endif
	if (res == LIBDEFLATE_INSUFFICIENT_SPACE) {
		g_decoder_stat_qoiz_oversize++;
		return 0;
	}
	if (res != LIBDEFLATE_SUCCESS) {
		g_decoder_stat_qoiz_bad++;
		return 0;
	}
	return out_len;
#else
	size_t in_len = in_size;
	size_t out_len = sizeof(qoiz_buf);
	tinfl_status st;

	tinfl_init(&qoiz_inflator);
	/* no TINFL_FLAG_HAS_MORE_INPUT: the transfer is the whole stream (a host
	 * that pads to an even length leaves a byte after the final block, which
	 * the inflater never asks for) */
	st = tinfl_decompress(&qoiz_inflator, in, &in_len, qoiz_buf, qoiz_buf,
	                      &out_len, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
#if DECODER_STATS
	g_qoiz_stat_in_bytes += in_len;
	g_qoiz_stat_out_bytes += out_len;
#endif
	if (st == TINFL_STATUS_HAS_MORE_OUTPUT) {
		g_decoder_stat_qoiz_oversize++;
		return 0;
	}
	if (st != TINFL_STATUS_DONE) {
		g_decoder_stat_qoiz_bad++;
		return 0;
	}
	return out_len;
#endif
}

void qoiz_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *qoiz_data, u32 qoiz_size)
{
	size_t qoi_len;
#if DECODER_STATS
	u32 t0 = time_us_32();
#endif

	if (qoiz_data == NULL || qoiz_size == 0)
		return;

	qoi_len = qoiz_inflate(qoiz_data, qoiz_size);
#if DECODER_STATS
	g_qoiz_stat_inflate_us += time_us_32() - t0;
#endif
	if (qoi_len == 0)
		return;

	qoi_drawimg(xs, ys, xe, ye, qoiz_buf, qoi_len);
}
#endif /* DECODER_USE_QOIZ */


/*
 * JPEG decoding and the TFT flush must NOT run on the USB interrupt stack:
 * the decode path needs a large stack and holding the USB IRQ for tens of
 * milliseconds wedges the USB controller and corrupts the interrupt stack.
 * The USB ISR therefore just copies the received frame into a slot and wakes
 * a dedicated decoder task which does the actual work.
 *
 * How many slots decides whether the decoder is visible at all.  EP1 is armed
 * only while a slot is free, so the device can receive the next band while the
 * current one is being decoded only if there is a slot for it: the pipeline is
 * slots - 1 bands deep, and anything the decoder spends beyond that shows up in
 * the host's frame time.
 *
 * Measured on an RP2350 at 225 MHz, QOI+deflate level 6, a full 480x320 screen,
 * the same instrument for every row (notes/decoders.md).  The baseline is QOI in
 * the same session, 93893 B in 101.06 ms:
 *
 *   slots  bytes  frame     vs QOI   the extra is
 *   2      68874   78.0 ms  -22.8%   ~4 ms of decode, still on the critical path
 *   3      68874   73.7 ms  -27.1%   nothing -- 73.6 ms is the link with no
 *                                    decoder at all (PUD_EP1_SINK)
 *
 * One slot holds any transfer the control stage accepts, so the third slot costs
 * PUD_MAX_TRANSFER: 32 KB on an RP2040, 64 KB on an RP2350.  Both still fit
 * (measured: RP2350 .data+.bss 504 KB of 512 KB with four, 440 KB with three;
 * that build links and then has no room for the stacks, so four is out of reach).
 */
#define DECODER_FRAME_SLOTS 3
/* One slot has to hold any transfer the control stage accepts, so it is sized
 * by the same per-board constant as ep1_read_buffer (PUD_MAX_TRANSFER, see
 * usbd_vendor.h). */
#define DECODER_FRAME_MAX PUD_MAX_TRANSFER

_Static_assert(DECODER_FRAME_MAX >= PUD_MAX_TRANSFER,
               "a frame slot must be able to hold a whole EP1 transfer");

struct decoder_frame {
	u16 xs, ys, xe, ye;
	u32 size;
	/* Position of this band in the accepted sequence.  DECODER_TYPE 6 needs
	 * it: the payload says which band its dictionary was built from, and the
	 * slot compares that against what it actually holds. */
	u32 serial;
	u8 busy;
	u8 data[DECODER_FRAME_MAX];
};

static struct decoder_frame s_frames[DECODER_FRAME_SLOTS];
/*
 * Slots are handed out lowest-free-first and drained in index order (see
 * decoder_submit_frame() and decoder_task()).  "The band submitted
 * DECODER_FRAME_SLOTS ago" therefore only holds while the pipeline is
 * saturated; a round-robin fill/drain cursor pair was tried and stalled the
 * pipeline under a full-screen load (303 submitted, 300 drawn, decoder_task
 * blocked), so DECODER_TYPE 6 finds its dictionary by the serial the payload
 * names instead of by slot arithmetic -- notes/decoders.md and notes/todo.md
 * item 13.
 */

#if DECODER_TYPE == DECODER_USE_QOID
/*
 * QOI + cross-frame dictionary deflate (experimental, DECODER_TYPE 6).
 *
 * The host QOI-encodes a band as for DECODER_TYPE 3 and then raw-deflates it
 * with `zdict` set to a band the device is *known to still hold*, so matches
 * reach backwards into the previous content instead of starting from nothing.
 * Measured on the host: a dirty band costs 32744 -> 18939 B (-42.2%) when a lot
 * changed and 32744 -> 6899 B (-78.9%) when little did, and one band with a UI
 * element appearing in it 11270 -> 3464 B (-69%) -- the redundancy a desktop
 * has between frames, which neither QOI nor frame-internal deflate can see.
 *
 * The device side is src/decoders/tinyd/: a compact decoder that decodes stored
 * and *fixed Huffman* blocks (which is what strategy=Z_FIXED produces, +11.0%
 * bytes on the measured band) and takes its history from the caller's buffer
 * rather than carrying a 32 KB window.  Measured on the same RP2350, same
 * payload, same clocks: 15.90 against zlib's 15.52 cycles per output byte, for
 * 2.4 KB of code instead of 14.1 KB plus 40 KB of window and state.
 *
 * One window per frame slot, laid out [history][output].  A band lands in the
 * lowest free slot, so the history a host can count on is "whatever this slot
 * decoded last time" and the host has to find that out from the device rather
 * than compute it -- notes/decoders.md and notes/todo.md item 13.  Deliberately
 * *not* a whole previous frame: a band's QOI stream is ~11.7 KB of the measured
 * desktop, ~94 KB for the full screen, and the build only has ~150 KB spare.
 *
 * The dictionary is an input the decoder cannot check: tinyd treats the
 * caller's buffer as history, so a wrong or stale dictionary still returns
 * "decoded 11270 bytes" and paints garbage (measured, notes/decoders.md).  The
 * protocol therefore carries the identity of the band the host built the
 * dictionary from in the payload's sub-header, and a band is refused unless this
 * slot holds exactly that band and exactly that many history bytes.  Refusing is
 * counted and visible; painting wrong pixels is not.
 */
#include "tinyd.h"

#define QOID_MAGIC 0x44445550u /* 'PUDD' in little-endian byte order */
#define QOID_F_DELTA 0x0001u
#define QOID_F_KEYFRAME 0x0002u
#define QOID_HDR_SIZE 16u

/* The sub-header lives inside the payload, not in struct pud_ep1_header: that
 * one is shared by every decoder and is a protocol field, so extending it would
 * drag the driver and both notes/usb-protocol.md along for one experiment. */
struct qoid_header {
	u32 magic;
	u16 flags;
	u16 reserved;
	u32 dict_serial; /* band the dictionary was built from */
	u32 dict_len; /* dictionary bytes the encoder actually used */
};

static u8 s_qoid_win[DECODER_FRAME_SLOTS][PUD_DELTA_WIN]
	__attribute__((aligned(4)));
/* What each window holds: which band's QOI stream, and how long it is. */
static u32 s_qoid_serial[DECODER_FRAME_SLOTS];
static u32 s_qoid_hist_len[DECODER_FRAME_SLOTS];
static u8 s_qoid_have[DECODER_FRAME_SLOTS];

volatile u32 g_decoder_stat_qoid_bad; /* malformed header, or tinyd refused */
volatile u32 g_decoder_stat_qoid_mismatch; /* dictionary is not what it claims */
volatile u32 g_decoder_stat_qoid_oversize; /* band does not fit the window */

/*
 * Forensics for the last band this decoder refused.  Refusals are rare (2 in
 * 14000 sends of ten fixed payloads, measured 2026-10-02) and a counter alone
 * does not say whether the device decoded the bytes that were sent or bytes
 * that were mangled on the way in, so the evidence has to survive until a
 * debugger reads it: what the payload fingerprinted as, where it landed, and
 * what tinyd made of it.  `sum` is an order-independent byte sum, so a slot
 * holding half of one band and half of another still matches nothing on the
 * host.  Written only from qoid_drawimg(), which runs in the decoder task, so
 * one writer and no locking.
 */
volatile struct {
	u32 count; /* bands refused so far (all three counters together) */
	u32 rc; /* tinyd result, or a marker for the stage that refused */
	u32 size; /* payload bytes as received */
	u32 slot; /* frame slot it arrived in */
	u32 sum; /* byte sum over the payload */
	u32 blk[8]; /* FNV-1a over each 1 KB block: where the bytes stop matching */
	u8 head[16]; /* first bytes, the sub-header included */
	u8 tail[16]; /* last bytes */
} g_qoid_reject;

#define QOID_REJ_SHORT 0x100u /* payload shorter than the sub-header */
#define QOID_REJ_MAGIC 0x101u /* sub-header magic is not QOID_MAGIC */
#define QOID_REJ_WINDOW 0x102u /* no window holds the named dictionary */

static void qoid_record_reject(u32 rc, const u8 *data, u32 size, int slot)
{
	u32 sum = 0;
	u32 i;

	for (i = 0; i < size; i++)
		sum += data[i];
	for (i = 0; i < sizeof(g_qoid_reject.blk) / sizeof(g_qoid_reject.blk[0]); i++) {
		u32 h = 2166136261u; /* FNV-1a, same constant the host recomputes */
		u32 j;
		u32 end = (i + 1) * 1024u;

		if (end > size)
			end = size;
		for (j = i * 1024u; j < end; j++)
			h = (h ^ data[j]) * 16777619u;
		g_qoid_reject.blk[i] = h;
	}
	g_qoid_reject.rc = rc;
	g_qoid_reject.size = size;
	g_qoid_reject.slot = (u32)slot;
	g_qoid_reject.sum = sum;
	for (i = 0; i < sizeof(g_qoid_reject.head); i++)
		g_qoid_reject.head[i] = i < size ? data[i] : 0;
	for (i = 0; i < sizeof(g_qoid_reject.tail); i++)
		g_qoid_reject.tail[i] = size >= sizeof(g_qoid_reject.tail) + i ?
		                                data[size - sizeof(g_qoid_reject.tail) + i] :
		                                0;
	g_qoid_reject.count++;
}
#if DECODER_STATS
volatile u32 g_qoid_stat_inflate_us;
volatile u32 g_qoid_stat_hist_bytes;
volatile u32 g_qoid_stat_out_bytes;
#endif

void qoid_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *data, u32 size, int slot,
                  u32 serial)
{
	const struct qoid_header *h;
	u8 *win;
	size_t dict_len = 0, out_len = 0;
	u32 cap = (u32)sizeof(s_qoid_win[0]) / 2;
	/* the window this band decodes into and leaves its history in: this band's
	 * own slot for a keyframe, the window that holds the dictionary for a
	 * delta */
	int win_idx = slot;
	int rc;
#if DECODER_STATS
	u32 t0 = time_us_32();
#endif

	if (data == NULL || slot < 0 || slot >= DECODER_FRAME_SLOTS)
		return;
	if (size < QOID_HDR_SIZE) {
		g_decoder_stat_qoid_bad++;
		qoid_record_reject(QOID_REJ_SHORT, data, size, slot);
		return;
	}

	h = (const struct qoid_header *)data;
	if (h->magic != QOID_MAGIC) {
		g_decoder_stat_qoid_bad++;
		qoid_record_reject(QOID_REJ_MAGIC, data, size, slot);
		return;
	}

	/*
	 * Decode against whichever window holds the band the payload names, not
	 * against this band's own slot.  The frame slot a band lands in depends on
	 * how full the pipeline was, so a host cannot predict it (measured: with a
	 * slow host every band lands in one slot, and a host that assumed round
	 * robin had 8 of 15 deltas refused); naming the band instead makes the
	 * dictionary the only thing that matters.  Decoding is sequential in one
	 * task, so two bands cannot share a window concurrently.
	 */
	if (h->flags & QOID_F_DELTA) {
		for (win_idx = 0; win_idx < DECODER_FRAME_SLOTS; win_idx++) {
			if (s_qoid_have[win_idx] &&
			    s_qoid_serial[win_idx] == h->dict_serial &&
			    s_qoid_hist_len[win_idx] == h->dict_len)
				break;
		}
		if (win_idx == DECODER_FRAME_SLOTS) {
			g_decoder_stat_qoid_mismatch++;
			qoid_record_reject(QOID_REJ_WINDOW, data, size, slot);
			return;
		}
		dict_len = h->dict_len;
	}

	win = s_qoid_win[win_idx];

	rc = tinyd_inflate(data + QOID_HDR_SIZE, size - QOID_HDR_SIZE, win,
	                   dict_len, cap, &out_len);
	if (rc != TINYD_OK) {
		/* A band that does not fit its window is the host's sizing
		 * mistake, not a corrupt stream: keep the two apart. */
		if (rc == TINYD_ERR_OVERFLOW)
			g_decoder_stat_qoid_oversize++;
		else
			g_decoder_stat_qoid_bad++;
		qoid_record_reject((u32)rc, data, size, slot);
		return;
	}

#if DECODER_STATS
	g_qoid_stat_inflate_us += time_us_32() - t0;
	g_qoid_stat_hist_bytes += (u32)dict_len;
	g_qoid_stat_out_bytes += (u32)out_len;
#endif

	/* qoi_drawimg() reads this buffer synchronously and writes the panel from
	 * its own band buffers, so the memo below cannot race an in-flight flush
	 * (the same reason DECODER_TYPE 5 reuses qoiz_buf every frame). */
	qoi_drawimg(xs, ys, xe, ye, win + dict_len, (u32)out_len);

	/* The band just decoded becomes this slot's history, whatever it was
	 * encoded against -- including a keyframe, which is what makes a host
	 * able to re-sync after any disagreement. */
	memmove(win, win + dict_len, out_len);
	s_qoid_hist_len[win_idx] = (u32)out_len;
	s_qoid_serial[win_idx] = serial;
	s_qoid_have[win_idx] = 1;
}

/* PUD_CMD_GET_QOID: which band each window holds.  Separate from the window
 * search on purpose -- asking is one change, using the answer to pick a
 * dictionary is another, and doing both at once is how the previous attempt
 * ended up reverted with 3 bands submitted and none drawn. */
void qoid_read_state(struct pud_qoid_state *st)
{
	int i;

	memset(st, 0, sizeof *st);
	st->magic = PUD_QOID_MAGIC;
	st->slots = DECODER_FRAME_SLOTS;
	st->window = PUD_DELTA_WIN;
	_Static_assert(DECODER_FRAME_SLOTS <= PUD_QOID_WINDOWS,
	               "struct pud_qoid_state has to cover every window");
	for (i = 0; i < DECODER_FRAME_SLOTS; i++) {
		st->valid[i] = s_qoid_have[i] ? 1 : 0;
		st->serial[i] = s_qoid_serial[i];
		st->len[i] = s_qoid_hist_len[i];
	}
}
#else
/* No dictionary windows in this build, but the query still has to answer
 * something a host can recognise: magic plus zero slots is that. */
void qoid_read_state(struct pud_qoid_state *st)
{
	memset(st, 0, sizeof *st);
	st->magic = PUD_QOID_MAGIC;
}
#endif /* DECODER_USE_QOID */
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

	/*
	 * Lowest free slot, which is *not* the round-robin rotation DECODER_TYPE 6
	 * would like: its dictionary lives in the slot, so the host would like to
	 * know which band is resident where.  Slot k only holds the band submitted
	 * k % DECODER_FRAME_SLOTS ago while the pipeline is saturated; a host slow
	 * enough to keep one band in flight puts every band in slot 0 (measured:
	 * 8 of 15 deltas refused).  A fill/drain cursor pair was tried and shown
	 * to stall the pipeline under a full-screen load (303 submitted, 300
	 * drawn, decoder_task blocked), so the device now finds the dictionary by
	 * the serial the payload names -- see notes/decoders.md and notes/todo.md
	 * item 13.
	 */
	for (i = 0; i < DECODER_FRAME_SLOTS; i++) {
		if (!s_frames[i].busy) {
			s_frames[i].xs = xs;
			s_frames[i].ys = ys;
			s_frames[i].xe = xe;
			s_frames[i].ye = ye;
			s_frames[i].size = size;
			s_frames[i].serial = g_decoder_stat_submitted - 1;
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
	/* The logo is baked for the orientation this firmware was built in: it is
	 * one image, bands and all, with no second copy for the other way round.
	 * A host that rotated the panel at runtime (PUD_CMD_SET_PARAM) gets no
	 * logo rather than a rotated one -- the first frame it sends is what it
	 * asked for anyway. */
	if (g_pud_data.disp.rotation != TFT_ROTATION)
		return;

#if DECODER_TYPE == DECODER_USE_LZ4
	const u8 *p = (const u8 *)bootlogo;
	u32 count = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
	            ((u32)p[3] << 24);
	u32 rows;
	u32 i;

	if (count == 0 || count > TFT_VER_RES || TFT_VER_RES % count != 0)
		return; /* not a container: nothing sane to draw */
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
		            (u16)(i + 1u == count ? TFT_VER_RES - 1 :
		                                    y + rows - 1),
		            (u8 *)p + start, end - start);
	}
#elif DECODER_TYPE == DECODER_USE_QOIZ || DECODER_TYPE == DECODER_USE_QOID
	/* stored as plain QOI, see the bootlogo.h include */
	qoi_drawimg(0, 0, TFT_HOR_RES - 1, TFT_VER_RES - 1, (uint8_t *)bootlogo,
	            sizeof(bootlogo));
#else
	decoder_drawimg(0, 0, TFT_HOR_RES - 1, TFT_VER_RES - 1,
	                (uint8_t *)bootlogo, sizeof(bootlogo));
#endif
}

static void decoder_task(void *param)
{
	int i;

	(void)param;

	/* Whatever the host set over USB that has to reach the panel (a rotation:
	 * the MADCTL write is not something to do in the USB interrupt) is applied
	 * here, before anything is drawn -- otherwise the logo and the first frame
	 * would be drawn in the orientation the panel still had. */
	pud_params_flush_display();

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
		int slot = -1;

		/* Take stock before sleeping.  The submit side signals a binary
		 * semaphore, so two frames arriving close together can collapse
		 * into a single give; scanning the slots first draws the second
		 * frame now instead of after the 200 ms wait below. */
		for (i = 0; i < DECODER_FRAME_SLOTS; i++) {
			if (s_frames[i].busy) {
				slot = i;
				break;
			}
		}

		if (slot < 0) {
			/* Nothing queued.  The timed wait doubles as the EP1
			 * caretaker: it drops a transfer whose host went away
			 * and re-arms a read that was lost. */
			if (xSemaphoreTake(s_decoder_sem,
			                   pdMS_TO_TICKS(EP1_POLL_PERIOD_MS)) != pdTRUE)
				usbd_vendor_ep1_poll();
			/* a rotation asked for while idle still has to land */
			pud_params_flush_display();
			continue;
		}

		/* Before this frame, not after: a rotation the host asked for
		 * arrives together with the mode change that follows it, and the
		 * frame it sends next is already in the new orientation. */
		pud_params_flush_display();

		decoder_drawimg_slot(s_frames[slot].xs, s_frames[slot].ys,
		                     s_frames[slot].xe, s_frames[slot].ye,
		                     s_frames[slot].data, s_frames[slot].size, slot,
		                     s_frames[slot].serial);
		s_frames[slot].busy = 0;
		g_decoder_stat_drawn++;
		/* A slot is free again: re-arm EP1 if the host's request was
		 * deferred. */
		usbd_vendor_ep1_tick();
	}
}

/* Indexed by DECODER_TYPE.  The numbering must not shift: the device reports
 * this value to the host through PUD_CMD_GET_CAPS. */
static char *decoder_names[] = { "tjpgd", "JPEGDEC", "LZ4",
	                         "QOI",   "RLE",
#if PUD_INFLATE == 2
	                         "QOI+deflate (libdeflate)"
#else
	                         "QOI+deflate (tinfl)"
#endif
	,
	                         "QOI+deflate+dict"
};
_Static_assert(DECODER_TYPE < sizeof(decoder_names) / sizeof(decoder_names[0]),
               "decoder_names[] has to cover every DECODER_TYPE");

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
