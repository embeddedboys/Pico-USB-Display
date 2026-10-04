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

#ifndef __DECODER_INTERNAL_H
#define __DECODER_INTERNAL_H

#include "decoder.h"
#include "pud.h"          /* PUD_EP1_HEADER_SIZE */
#include "usbd_vendor.h"  /* PUD_MAX_TRANSFER */


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
extern volatile u32 g_qoi_stat_draw_us; /* whole qoi_drawimg() */
extern volatile u32 g_qoi_stat_flush_us; /* time inside tft_video_flush() */
extern volatile u32 g_qoi_stat_pixels; /* pixels flushed */
extern volatile u32 g_qoi_stat_calls; /* qoi_flush() calls */
extern volatile u32 g_qoi_stat_frames; /* qoi_drawimg() calls */

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
 * The largest band the host may send, in pixels -- one pixel more than the
 * device's own rounding, because the host rounds
 * ((min(65535, frame_max) - PUD_EP1_HEADER_SIZE - 16) / 3) while the device
 * rounds the same expression without the min().  The 16 bytes are the codec's
 * own framing (QOI's header plus end marker); the 12 are the EP1 header in
 * front of every payload.
 *
 * LZ4 sizes its single band buffer with this, and QOI its accumulation
 * buffer, so the name says what the number is rather than which codec needed it
 * first.
 */
#define PUD_BAND_MAX_PIXELS \
	(((PUD_MAX_TRANSFER - PUD_EP1_HEADER_SIZE - 16) / 3) + 1)

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

#endif /* __DECODER_INTERNAL_H */
