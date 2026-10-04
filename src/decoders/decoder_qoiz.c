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
