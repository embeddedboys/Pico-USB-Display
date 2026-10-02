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
 * RGB565 RLE (vendored in src/decoders/rle/).
 *
 * Same shape as the QOI path: the library decodes into two ping-pong buffers
 * and calls back once per batch, so a batch can be pushed to the panel while
 * the next one is decoded.  It keeps no state of its own, so unlike the JPEG
 * decoders there is no workspace to account for.
 */
#include "rgb565_rle.h"

#define RLE_BUF_ROWS 8

/* RLE_BUF_ROWS panel rows per accumulation buffer, two ping-ponged; see
 * QOI_BATCH_PIXELS above for why the width comes from the panel. */
#define RLE_BATCH_PIXELS (TFT_HOR_RES * RLE_BUF_ROWS)
static uint16_t rle_buf_a[RLE_BATCH_PIXELS];
#if PUD_DECODER_PINGPONG
static uint16_t rle_buf_b[RLE_BATCH_PIXELS];
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

	if (rle_data == NULL || rle_size == 0 || width == 0 || width > TFT_HOR_RES)
		return;

	ctx.ox = xs;
	ctx.oy = ys;

	/* whole rows per batch, capped at the static buffers */
	buf_cap = (size_t)width * RLE_BUF_ROWS;
	if (buf_cap > RLE_BATCH_PIXELS)
		buf_cap = RLE_BATCH_PIXELS;

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
