/* tinyd -- a tiny raw-deflate decoder with a preset window, for cross-frame
 * deltas on a microcontroller.
 *
 * This is not a general inflate: it decodes stored blocks and *fixed Huffman*
 * blocks only, which is what a host gets by asking zlib for
 * `strategy=Z_FIXED` (measured: +10.3% bytes, 0.8 ms/frame on the display's
 * frame budget, for a decoder a fifth of the size of a full one).  Dynamic
 * Huffman blocks are reported as an error rather than decoded, so a stream that
 * needs them fails loudly instead of silently producing rubbish.
 *
 * The point of it is the window: the caller passes one contiguous buffer whose
 * first `dict_len` bytes are *already known* -- the previous frame's band, in
 * this project's use -- and this stream decodes after them.  A match may reach
 * back into that dictionary, which is where a desktop's redundancy lives; no
 * extra window buffer is needed, which is what makes it affordable next to
 * zlib's inflate (32 KB window + ~7 KB state).
 */
#ifndef TINYD_H
#define TINYD_H

#include <stddef.h>
#include <stdint.h>

enum {
	TINYD_OK = 0,
	TINYD_ERR_FORMAT = -1,   /* not a fixed-Huffman or stored block */
	TINYD_ERR_TRUNCATED = -2,
	TINYD_ERR_DISTANCE = -3,  /* match reaches before the window start */
	TINYD_ERR_OVERFLOW = -4,  /* output does not fit out_max */
	TINYD_ERR_INCOMPLETE = -5 /* final block never arrived */
};

/* Decode `in` into win[dict_len .. dict_len + out_max), returning the number of
 * bytes produced in *out_len.  win[0 .. dict_len) is history, not output. */
int tinyd_inflate(const uint8_t *in, size_t in_len,
		  uint8_t *win, size_t dict_len, size_t out_max, size_t *out_len);

#endif /* TINYD_H */
