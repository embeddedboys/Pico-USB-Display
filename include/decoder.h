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

#ifndef __DECODER_H
#define __DECODER_H

#include <stdbool.h>

#include "JPEGDEC.h"

/* DECODER_TYPE values.  The numbering is part of the USB protocol (the device
 * reports it to the host through PUD_CMD_GET_CAPS), so do not renumber. */
#define DECODER_USE_TJPGD 0
#define DECODER_USE_JPEGDEC 1
#define DECODER_USE_LZ4 2
#define DECODER_USE_QOI 3
/* RGB565 RLE, the codec from the rgb565-rle library (see src/decoders/rle/).
 * Added after QOI, so it takes the next free number. */
#define DECODER_USE_RLE 4
/* QOI, then raw deflate (RFC 1951) over the QOI stream.  The device inflates the
 * transfer (tinfl, src/decoders/miniz/) and hands the result to the QOI decoder.
 * Experimental; see notes/decoders.md. */
#define DECODER_USE_QOIZ 5
/* QOI, then raw deflate over the QOI stream with the *previous frame's same
 * band* as preset dictionary -- a cross-frame delta.  The window is the
 * caller's buffer, decoded by src/decoders/tinyd/ (fixed Huffman + stored
 * blocks only).  Experimental; see notes/decoders.md. */
#define DECODER_USE_QOID 6

/*
 * CMake always defines this (DECODER_TYPE in CMakeLists.txt, default 3 = QOI),
 * so the fallback only applies to a build that bypassed it -- in which case it
 * has to agree with that default.  It used to say JPEGDEC, which would have
 * built a firmware the driver does not speak, silently, from the same source
 * tree the CMake build produces a working image from.
 */
#ifndef DECODER_TYPE
#define DECODER_TYPE DECODER_USE_QOI
#endif

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

extern void tjpgd_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *jpeg_data,
                          u32 jpeg_size);
extern void jpegdec_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *jpeg_data,
                            u32 jpeg_size);
extern void lz4_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *lz4_data,
                        u32 lz4_size);
extern void qoi_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *qoi_data,
                        u32 qoi_size);
extern void rle_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *rle_data,
                        u32 rle_size);
extern void qoiz_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *qoiz_data,
                         u32 qoiz_size);
/* DECODER_TYPE 6 keeps one dictionary window per frame slot -- the history for
 * a band is whatever that slot decoded last time -- so unlike every other
 * decoder here it has to know which slot the band arrived in.  `serial` is the
 * band's position in the accepted sequence; it is carried in the payload's
 * sub-header and checked against what the slot holds, because a wrong
 * dictionary decodes to plausible-looking garbage instead of failing
 * (notes/decoders.md). */
extern void qoid_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *qoid_data,
                         u32 qoid_size, int slot, u32 serial);
/* Answers PUD_CMD_GET_QOID: which band each dictionary window holds.  Defined
 * for every build -- a device without windows still has to answer, otherwise a
 * host cannot tell "no windows" from "command not understood". */
struct pud_qoid_state;
extern void qoid_read_state(struct pud_qoid_state *st);

extern void decoder_init(void);
extern void decoder_submit_frame(u16 xs, u16 ys, u16 xe, u16 ye, const u8 *data,
                                 u32 size);
extern bool decoder_slot_free(void);

#if DECODER_TYPE == DECODER_USE_TJPGD
#define decoder_drawimg(xs, ys, xe, ye, b, l) \
	tjpgd_drawimg(xs, ys, xe, ye, b, l)
#elif DECODER_TYPE == DECODER_USE_JPEGDEC
#define decoder_drawimg(xs, ys, xe, ye, b, l) \
	jpegdec_drawimg(xs, ys, xe, ye, b, l)
#elif DECODER_TYPE == DECODER_USE_LZ4
#define decoder_drawimg(xs, ys, xe, ye, b, l) lz4_drawimg(xs, ys, xe, ye, b, l)
#elif DECODER_TYPE == DECODER_USE_QOI
#define decoder_drawimg(xs, ys, xe, ye, b, l) qoi_drawimg(xs, ys, xe, ye, b, l)
#elif DECODER_TYPE == DECODER_USE_RLE
#define decoder_drawimg(xs, ys, xe, ye, b, l) rle_drawimg(xs, ys, xe, ye, b, l)
#elif DECODER_TYPE == DECODER_USE_QOIZ
#define decoder_drawimg(xs, ys, xe, ye, b, l) qoiz_drawimg(xs, ys, xe, ye, b, l)
#elif DECODER_TYPE == DECODER_USE_QOID
#define decoder_drawimg(xs, ys, xe, ye, b, l) \
	qoid_drawimg(xs, ys, xe, ye, b, l, 0, 0)
#else
#error "Invalid decoder type selected"
#endif /* DECODER_TYPE */

/*
 * The frame task knows which slot a band came out of; only DECODER_TYPE 6 cares
 * (it keeps its dictionary per slot).  Every other decoder ignores the extra
 * argument, so this stays a macro rather than a function.
 */
#if DECODER_TYPE == DECODER_USE_QOID
#define decoder_drawimg_slot(xs, ys, xe, ye, b, l, slot, serial) \
	qoid_drawimg(xs, ys, xe, ye, b, l, slot, serial)
#else
#define decoder_drawimg_slot(xs, ys, xe, ye, b, l, slot, serial) \
	decoder_drawimg(xs, ys, xe, ye, b, l)
#endif

#endif /* __UDD_DECODER_H */
