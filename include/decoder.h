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

#ifndef DECODER_TYPE
	#define DECODER_TYPE DECODER_USE_JPEGDEC
#endif

extern mutex_t decoder_mutex;
extern uint16_t decoder_x, decoder_y;

// extern JRESULT jd_getsize(uint16_t *w, uint16_t *h, const uint8_t jpeg_data[], uint32_t  data_size);
// extern JRESULT jd_drawjpg(int32_t x, int32_t y, const uint8_t jpeg_data[], uint32_t  data_size);
extern void jpegdec_drawjpg(int x, int y, uint8_t *jpeg_data, uint32_t jpeg_size);

extern void decoder_init(void);
extern void decoder_set_xy(int x, int y);

#if DECODER_TYPE == DECODER_USE_TJPGD
	#define decoder_drawimg(x, y, b, l) jd_drawjpg(x, y, b, l)
#elif DECODER_TYPE == DECODER_USE_JPEGDEC
	#define decoder_drawimg(x, y, b, l) jpegdec_drawjpg(x, y, b, l)
#else
	#error "Invalid decoder type selected"
#endif /* DECODER_TYPE */

#endif /* __UDD_DECODER_H */
