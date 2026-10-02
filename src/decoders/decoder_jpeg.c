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

struct jpegdec_data {
	JPEGIMAGE img;
	u8 options;
};
struct jpegdec_data g_jpegdec;

int draw_mcus(JPEGDRAW *pDraw)
{
	/* iWidth is the MCU-pitch padded width; only iWidthUsed pixels are
	 * valid for edge/cropped blocks. Flushing the padded width smears
	 * garbage over the right edge of partial updates (skewed image).
	 */
	int iWidth = pDraw->iWidthUsed;
	int iCount = iWidth * pDraw->iHeight * 2; /* sizeof(*pDraw->pPixels) */
	int xs = pDraw->x;
	int ys = pDraw->y;
	int xe = pDraw->x + iWidth - 1;
	int ye = pDraw->y + pDraw->iHeight - 1;

	tft_video_flush(xs, ys, xe, ye, pDraw->pPixels, iCount);
	return iCount;
}

void jpegdec_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *jpeg_data,
                     u32 jpeg_size)
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

/*
 * tjpgd (ChaN's TJpgDec R0.03, vendored in src/decoders/tjpgd/).
 *
 * tjpgdcnf.h sets JD_FORMAT 1, so the output callback receives RGB565 in the
 * same byte order the panel takes, and JD_FASTDECODE 2, which makes the
 * workspace TJPGD_WORKSPACE_SIZE (9.4 KB).  Both are static: the decoder
 * task's stack is only sized for call frames.
 *
 * tjpgd hands out one 8x8 block at a time, so flushing each block straight to
 * the panel would cost one address-window setup per block -- 2400 of them for
 * a 480x320 image.  Collect whole rows of blocks and flush one rectangle per
 * group instead, for the same reason the QOI path batches its output.
 *
 * Unlike QOI this decodes a whole image per call, so it is only useful for
 * whole-rectangle updates (the host sends a self-contained JPEG of the
 * rectangle, exactly like the JPEGDEC path).
 */
#include "tjpgd.h"

static uint8_t s_tjpgd_workspace[TJPGD_WORKSPACE_SIZE]
        __attribute__((aligned(4)));

/* Rows of blocks buffered before one flush; 8 panel rows at 2 B per pixel, so
 * 7.7 KB across the widest panel this firmware drives.  The buffer is always
 * indexed with the visible width as the row pitch, so a flush is a contiguous
 * run even for a narrower image. */
#define TJPGD_GROUP_ROWS 8
/* Sized by the larger of the two, because a 90 degree rotation asked for at
 * runtime swaps width and height: max() of the build pair is the same either way. */
static u16 s_tjpgd_rowbuf[MAX(TFT_HOR_RES, TFT_VER_RES) * TJPGD_GROUP_ROWS];

struct tjpgd_ctx {
	const u8 *data;
	u32 size;
	u32 index;
	int16_t x, y; /* where the image goes on the panel */
	u16 width; /* visible width of the image (row pitch of the buffer) */
	u16 group; /* row group currently buffered */
	u16 group_rows; /* how many rows of it are filled */
	bool group_valid;
};

static struct tjpgd_ctx s_tjpgd;

static size_t tjpgd_input(JDEC *jdec, uint8_t *buf, size_t len)
{
	(void)jdec;

	if (s_tjpgd.index + len > s_tjpgd.size)
		len = s_tjpgd.size - s_tjpgd.index;

	memcpy(buf, s_tjpgd.data + s_tjpgd.index, len);
	s_tjpgd.index += len;

	return len;
}

static void tjpgd_flush_group(void)
{
	int y = s_tjpgd.y + s_tjpgd.group * TJPGD_GROUP_ROWS;
	u16 rows = s_tjpgd.group_rows;

	if (!s_tjpgd.group_valid)
		return;
	s_tjpgd.group_valid = false;
	s_tjpgd.group_rows = 0;

	if (rows == 0 || y >= g_pud_data.disp.yres || s_tjpgd.width == 0)
		return;
	if (y + rows > g_pud_data.disp.yres)
		rows = g_pud_data.disp.yres - y;

	tft_video_flush(s_tjpgd.x, y, s_tjpgd.x + s_tjpgd.width - 1,
	                y + rows - 1, s_tjpgd_rowbuf, s_tjpgd.width * rows * 2);
}

static int tjpgd_output(JDEC *jdec, void *bitmap, JRECT *rect)
{
	/* Clip the padding tjpgd adds to the last MCU row and column before it
	 * reaches the panel -- the trap JPEGDEC's iWidthUsed exists for. */
	u16 pitch = rect->right - rect->left + 1;
	u16 bh = rect->bottom - rect->top + 1;
	u16 bw = pitch;
	u16 *px = (u16 *)bitmap;
	u16 r;

	if (rect->left + bw > jdec->width)
		bw = jdec->width - rect->left;
	if (rect->top + bh > jdec->height)
		bh = jdec->height - rect->top;
	if (bw > s_tjpgd.width)
		bw = s_tjpgd.width;
	if (bw == 0 || bh == 0)
		return 1;

	/* One call can cover a whole MCU, and a 4:2:0 MCU is 16 rows tall while
	 * the buffer holds TJPGD_GROUP_ROWS: walk the block row by row and flush
	 * every time it would leave the buffered group, so the buffer size does
	 * not depend on the JPEG's chroma subsampling. */
	for (r = 0; r < bh; r++) {
		u16 row = rect->top + r;
		u16 group = row / TJPGD_GROUP_ROWS;
		u16 row0 = row - group * TJPGD_GROUP_ROWS;

		if (s_tjpgd.group_valid && group != s_tjpgd.group)
			tjpgd_flush_group();

		s_tjpgd.group = group;
		s_tjpgd.group_valid = true;

		memcpy(&s_tjpgd_rowbuf[row0 * s_tjpgd.width + rect->left],
		       px + r * pitch, bw * 2);

		if (row0 + 1 > s_tjpgd.group_rows)
			s_tjpgd.group_rows = row0 + 1;
	}

	return 1; /* continue decoding */
}

void tjpgd_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *jpeg_data, u32 jpeg_size)
{
	JDEC jdec;
	JRESULT res;

	/* The image carries its own size; xe/ye only repeat it. */
	(void)xe;
	(void)ye;

	s_tjpgd.data = jpeg_data;
	s_tjpgd.size = jpeg_size;
	s_tjpgd.index = 0;
	s_tjpgd.x = xs;
	s_tjpgd.y = ys;
	s_tjpgd.group = 0;
	s_tjpgd.group_rows = 0;
	s_tjpgd.group_valid = false;

	memset(&jdec, 0, sizeof(jdec));
	jdec.swap = false; /* JD_FORMAT 1 already gives panel byte order */

	res = jd_prepare(&jdec, tjpgd_input, s_tjpgd_workspace,
	                 sizeof(s_tjpgd_workspace), NULL);
	if (res != JDR_OK) {
		printf("tjpgd: jd_prepare failed: %d\n", res);
		return;
	}

	/* Only the part that lands on the panel is worth buffering. */
	s_tjpgd.width = jdec.width;
	if (s_tjpgd.x + s_tjpgd.width > g_pud_data.disp.xres)
		s_tjpgd.width = g_pud_data.disp.xres - s_tjpgd.x;
	if (s_tjpgd.width > g_pud_data.disp.xres)
		s_tjpgd.width = g_pud_data.disp.xres;

	res = jd_decomp(&jdec, tjpgd_output, 0);
	tjpgd_flush_group();
	if (res != JDR_OK)
		printf("tjpgd: jd_decomp failed: %d\n", res);
}
