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
#include "lz4.h"
#include "decoder_internal.h"
#include "pud.h"

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
 */
static uint16_t lz4_band[PUD_BAND_MAX_PIXELS] __attribute__((aligned(4)));

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
