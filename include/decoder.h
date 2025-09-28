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

#include <pico/mutex.h>

// #include "tjpgd/tjpgd.h"
#include "JPEGDEC.h"

#define DECODER_USE_TJPGD   0
#define DECODER_USE_JPEGDEC 1
#define DECODER_USE_LZ4     2

#ifndef DECODER_TYPE
	#define DECODER_TYPE DECODER_USE_JPEGDEC
#endif

typedef unsigned short u16;

extern mutex_t decoder_mutex;
extern uint16_t decoder_xs, decoder_ys;
extern uint16_t decoder_xe, decoder_ye;

// extern JRESULT jd_getsize(uint16_t *w, uint16_t *h, const uint8_t jpeg_data[], uint32_t  data_size);
// extern JRESULT jd_drawimg(int32_t x, int32_t y, const uint8_t jpeg_data[], uint32_t  data_size);
extern void jpegdec_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, uint8_t *jpeg_data, uint32_t jpeg_size);
extern void lz4_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, uint8_t *lz4_data, uint32_t lz4_size);

extern void decoder_init(void);
extern void decoder_set_xy(u16 x, u16 y);
extern void decoder_set_window(u16 xs, u16 ys, u16 xe, u16 ye);

#if DECODER_TYPE == DECODER_USE_TJPGD
	#error "TJPGD decoder is unimplemented yet"
	// #define decoder_drawimg(xs, ys, xe, ye, b, l) jd_drawimg(xs, ys, xe, ye, b, l)
#elif DECODER_TYPE == DECODER_USE_JPEGDEC
	#define decoder_drawimg(xs, ys, xe, ye, b, l) jpegdec_drawimg(xs, ys, xe, ye, b, l)
#elif DECODER_TYPE == DECODER_USE_LZ4
	#define decoder_drawimg(xs, ys, xe, ye, b, l) lz4_drawimg(xs, ys, xe, ye, b, l)
#else
	#error "Invalid decoder type selected"
#endif /* DECODER_TYPE */

#endif /* __UDD_DECODER_H */
