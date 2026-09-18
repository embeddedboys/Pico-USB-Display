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
#define DECODER_USE_TJPGD   0
#define DECODER_USE_JPEGDEC   1
#define DECODER_USE_LZ4       2
#define DECODER_USE_QOI       3

#ifndef DECODER_TYPE
	#define DECODER_TYPE DECODER_USE_JPEGDEC
#endif

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

extern uint16_t decoder_xs, decoder_ys;
extern uint16_t decoder_xe, decoder_ye;

extern void tjpgd_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *jpeg_data, u32 jpeg_size);
extern void jpegdec_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *jpeg_data, u32 jpeg_size);
extern void lz4_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *lz4_data, u32 lz4_size);
extern void qoi_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *qoi_data, u32 qoi_size);

extern void decoder_init(void);
extern void decoder_set_window(u16 xs, u16 ys, u16 xe, u16 ye);
extern void decoder_submit_frame(u16 xs, u16 ys, u16 xe, u16 ye,
				 const u8 *data, u32 size);
extern bool decoder_slot_free(void);

#if DECODER_TYPE == DECODER_USE_TJPGD
	#define decoder_drawimg(xs, ys, xe, ye, b, l) tjpgd_drawimg(xs, ys, xe, ye, b, l)
#elif DECODER_TYPE == DECODER_USE_JPEGDEC
	#define decoder_drawimg(xs, ys, xe, ye, b, l) jpegdec_drawimg(xs, ys, xe, ye, b, l)
#elif DECODER_TYPE == DECODER_USE_LZ4
	#define decoder_drawimg(xs, ys, xe, ye, b, l) lz4_drawimg(xs, ys, xe, ye, b, l)
#elif DECODER_TYPE == DECODER_USE_QOI
	#define decoder_drawimg(xs, ys, xe, ye, b, l) qoi_drawimg(xs, ys, xe, ye, b, l)
#else
	#error "Invalid decoder type selected"
#endif /* DECODER_TYPE */

#endif /* __UDD_DECODER_H */
