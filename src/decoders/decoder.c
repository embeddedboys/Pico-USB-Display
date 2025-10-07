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

#include <stdlib.h>

// #include "udd.h"
#include "tft.h"
#include "decoder.h"
#include "lz4.h"

mutex_t decoder_mutex;
uint16_t decoder_xs, decoder_ys;
uint16_t decoder_xe, decoder_ye;

/* for TJPGD lib */
// uint8_t workspace[TJPGD_WORKSPACE_SIZE] __attribute__((aligned(4)));

// unsigned int jd_input(JDEC* jdec, uint8_t* buf, unsigned int len)
// {
// 	struct tjpgd_data *tjpgd = &g_udd_data.tjpgd;

// 	memcpy(buf, (const uint8_t *)(tjpgd->array_data + tjpgd->array_index), len);
// 	tjpgd->array_index += len;

// 	return len;
// }

// int jd_output(JDEC* jdec, void* bitmap, JRECT* jrect)
// {
// 	struct tjpgd_data *tjpgd = &g_udd_data.tjpgd;

// 	int16_t  x = jrect->left + tjpgd->jpeg_x;
// 	int16_t  y = jrect->top  + tjpgd->jpeg_y;
// 	uint16_t w = jrect->right  - jrect->left;
// 	uint16_t h = jrect->bottom - jrect->top;

// 	// pr_debug("%d, %d, %d, %d\n", jrect->top, jrect->left, jrect->bottom, jrect->right);
// 	// pr_debug("%d, %d, %d, %d\n", x, y, w, h);

// 	ili9488_video_flush(x, y, x + w, y + h, (uint16_t *)bitmap, (w + 1) * (h + 1));
// }

// JRESULT jd_getsize(uint16_t *w, uint16_t *h, const uint8_t jpeg_data[], uint32_t  data_size)
// {
// 	struct tjpgd_data *tjpgd = &g_udd_data.tjpgd;
// 	JRESULT jresult = JDR_OK;
// 	JDEC jdec;

// 	tjpgd->array_index = 0;
// 	tjpgd->array_data  = jpeg_data;
// 	tjpgd->array_size  = data_size;

// 	jresult =  jd_prepare(&jdec, jd_input, workspace, TJPGD_WORKSPACE_SIZE, 0);
// 	if (jresult == JDR_OK) {
// 		*w = jdec.width;
// 		*h = jdec.height;
// 	}

// 	return jresult;
// }

// JRESULT jd_drawimg(int32_t x, int32_t y, const uint8_t jpeg_data[], uint32_t  data_size)
// {
// 	struct tjpgd_data *tjpgd = &g_udd_data.tjpgd;
// 	JRESULT jresult = JDR_OK;
// 	JDEC jdec;

// 	tjpgd->array_index = 0;
// 	tjpgd->array_data  = jpeg_data;
// 	tjpgd->array_size  = data_size;

// 	tjpgd->jpeg_x = x;
// 	tjpgd->jpeg_y = y;

// 	jdec.swap = false;

// 	jresult = jd_prepare(&jdec, jd_input, workspace, TJPGD_WORKSPACE_SIZE, 0);
// 	if (jresult == JDR_OK) {
// 		jresult = jd_decomp(&jdec, jd_output, 0);
// 	}

// 	return jresult;
// }

struct jpegdec_data {
	JPEGIMAGE img;
	u8 options;
};
struct jpegdec_data g_jpegdec;

int draw_mcus(JPEGDRAW *pDraw)
{
	int iCount = pDraw->iWidth * pDraw->iHeight * 2;  /* sizeof(*pDraw->pPixels) */
	int xs = pDraw->x;
	int ys = pDraw->y;
	int xe = pDraw->x + pDraw->iWidth - 1;
	int ye = pDraw->y + pDraw->iHeight - 1;

	tft_video_flush(xs, ys, xe, ye, pDraw->pPixels, iCount);
	return iCount;
}

void jpegdec_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *jpeg_data, u32 jpeg_size)
{
	static struct jpegdec_data *jpegdec = &g_jpegdec;
	int ret;

	JPEG_setPixelType(&jpegdec->img, RGB565_LITTLE_ENDIAN);

	ret = JPEG_openRAM(&jpegdec->img, jpeg_data, jpeg_size, draw_mcus);
	if (ret) {
		// pr_debug("Successfully opened JPEG image\n");
		// pr_debug("Image size: %d x %d, orientation: %d, bpp: %d\n",
		// 	JPEG_getWidth(&jpegdec->img),
		// 	JPEG_getHeight(&jpegdec->img),
		// 	JPEG_getOrientation(&jpegdec->img),
		// 	JPEG_getBpp(&jpegdec->img)
		// );
		JPEG_decode(&jpegdec->img, xs, ys, jpegdec->options);
	}
}

void lz4_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *lz4_data, u32 lz4_size)
{
	printf("%s, size :%d\n", __func__, lz4_size);
	char *lz4_workspace;
	int max_compressed_size = LZ4_compressBound(TFT_HOR_RES * TFT_VER_RES * 2);
	printf("%s, lz4 compreess boud: %d\n", __func__, max_compressed_size);

	lz4_workspace = (char *)malloc(max_compressed_size);
	if (lz4_workspace == NULL) {
		printf("%s, malloc workspace failed!", __func__);
		return;
	}

	int decompressed_size = LZ4_decompress_safe((char *)lz4_data, (char *)lz4_workspace, lz4_size, max_compressed_size);
	printf("%s, decompressed_size: %d\n", __func__, decompressed_size);
	if (decompressed_size < 0)
		goto decompress_failed;

	tft_video_flush(xs, ys, xe, ye, lz4_workspace, decompressed_size);
decompress_failed:
	free(lz4_workspace);
}

void decoder_set_xy(u16 x, u16 y)
{
	mutex_enter_blocking(&decoder_mutex);
	decoder_xs = x;
	decoder_ys = y;
	mutex_exit(&decoder_mutex);
}

void decoder_set_window(u16 xs, u16 ys, u16 xe, u16 ye)
{
	mutex_enter_blocking(&decoder_mutex);
	decoder_xs = xs;
	decoder_ys = ys;
	decoder_xe = xe;
	decoder_ye = ye;
	mutex_exit(&decoder_mutex);
}

static char *decoder_names[] = {
	"tjpgd",
	"JPEGDEC",
	"LZ4"
};

void decoder_init(void)
{
	mutex_init(&decoder_mutex);
	printf("Decoder type: %s\n", decoder_names[DECODER_TYPE]);
}
