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
