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

#include <string.h>

#include "pico/time.h"

#include "tft.h"
#include "decoder.h"
#include "decoder_internal.h"
#include "pud.h"

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

/*
 * Whole panel rows per accumulation buffer, two of them ping-ponged.  The width
 * is the panel's rather than a literal: a narrower panel would otherwise
 * reserve memory it cannot use, and a wider one would overflow the buffer that
 * the band bound in qoi_drawimg() promises to respect.
 */
#define QOI_BATCH_PIXELS (TFT_HOR_RES * QOI_BUF_ROWS)
static uint16_t qoi_buf_a[QOI_BATCH_PIXELS];
#if PUD_DECODER_PINGPONG
static uint16_t qoi_buf_b[QOI_BATCH_PIXELS];
#endif

#if QOI_NONCALLBACK >= 2
/* Two whole-band buffers: one is being written to the panel while the next
 * band is decoded into the other (see qoi_drawimg). */
/*
 * The DECODER_STATS counters themselves.  decoder_internal.h declares them
 * extern because the QOI, RLE, QOIZ and QOID paths all accumulate into them,
 * so exactly one translation unit has to own the storage -- and it has to be
 * one that is always compiled, which this one is (every build includes the QOI
 * source list, whatever DECODER_TYPE selects).
 */
#if DECODER_STATS
volatile u32 g_qoi_stat_draw_us;  /* whole qoi_drawimg() */
volatile u32 g_qoi_stat_flush_us; /* time inside tft_video_flush() */
volatile u32 g_qoi_stat_pixels;   /* pixels flushed */
volatile u32 g_qoi_stat_calls;    /* qoi_flush() calls */
volatile u32 g_qoi_stat_frames;   /* qoi_drawimg() calls */
#endif

static uint16_t qoi_band[2 * PUD_BAND_MAX_PIXELS] __attribute__((aligned(4)));
static unsigned qoi_band_next;
#elif QOI_NONCALLBACK
/* One whole-band buffer, without the overlap (for A/B measurements). */
static uint16_t qoi_band[PUD_BAND_MAX_PIXELS] __attribute__((aligned(4)));
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

	if (qoi_data == NULL || qoi_size == 0 || width == 0 || width > TFT_HOR_RES)
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

		if (rect_px <= PUD_BAND_MAX_PIXELS) {
#if QOI_NONCALLBACK >= 2
			uint16_t *buf =
			        qoi_band + qoi_band_next * PUD_BAND_MAX_PIXELS;
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
	if (buf_cap > QOI_BATCH_PIXELS)
		buf_cap = QOI_BATCH_PIXELS;

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
