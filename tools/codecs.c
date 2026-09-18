/*
 * Codec dispatch for the host-side conversion tool.
 *
 * SPDX-License-Identifier: MIT
 */

#include "codecs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

#include "lz4.h"
#include "rgb565_qoi.h"
#include "rgb565_rle.h"

static const char *const codec_names[PUD_CODEC_COUNT] = {
	[PUD_CODEC_QOI] = "qoi",
	[PUD_CODEC_RLE] = "rle",
	[PUD_CODEC_LZ4] = "lz4",
	[PUD_CODEC_JPEG] = "jpeg",
};

/* DECODER_TYPE as reported through PUD_CMD_GET_CAPS.  tjpgd and JPEGDEC are
 * both JPEG streams; the tool writes one JPEG and either decoder takes it. */
static const int codec_decoder_type[PUD_CODEC_COUNT] = {
	[PUD_CODEC_QOI] = 3,
	[PUD_CODEC_RLE] = 4,
	[PUD_CODEC_LZ4] = 2,
	[PUD_CODEC_JPEG] = 1,
};

int pud_codec_by_name(const char *name)
{
	int i;

	if (name == NULL)
		return -1;

	for (i = 0; i < PUD_CODEC_COUNT; i++) {
		if (strcmp(name, codec_names[i]) == 0)
			return i;
	}

	return -1;
}

const char *pud_codec_name(int codec)
{
	if (codec < 0 || codec >= PUD_CODEC_COUNT)
		return "?";

	return codec_names[codec];
}

int pud_codec_decoder_type(int codec)
{
	if (codec < 0 || codec >= PUD_CODEC_COUNT)
		return -1;

	return codec_decoder_type[codec];
}

size_t pud_codec_max_size(int codec, int width, int height)
{
	size_t pixels;

	if (width <= 0 || height <= 0)
		return 0u;

	pixels = (size_t)width * (size_t)height;
	if (pixels / (size_t)width != (size_t)height)
		return 0u;

	switch (codec) {
	case PUD_CODEC_QOI:
		return rgb565_qoi_max_compressed_size(pixels);
	case PUD_CODEC_RLE:
		return rgb565_rle_max_compressed_size(pixels);
	case PUD_CODEC_LZ4: {
		int bound;

		if (pixels > (size_t)INT32_MAX / 2u)
			return 0u;
		bound = LZ4_compressBound((int)(pixels * 2u));
		return bound > 0 ? (size_t)bound : 0u;
	}
	case PUD_CODEC_JPEG:
		/* stb reports failure instead of overrunning the buffer, so this
		 * only has to be generous enough not to fail in practice. */
		return pixels * 3u + 65536u;
	default:
		return 0u;
	}
}

/*
 * RGB888 -> RGB565, truncating rather than rounding: this is what
 * scripts/pud_usb.py does (`(r >> 3) << 11 | ...`), so for the same lossless
 * source both host paths pack the same pixels and therefore produce
 * byte-identical streams.  The difference to rounding is at most one LSB and
 * invisible on the panel; keeping the two in step is what makes them
 * comparable.
 */
uint16_t pud_rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint16_t)(((uint16_t)(r & 0xF8u) << 8) |
	                  ((uint16_t)(g & 0xFCu) << 3) | (uint16_t)(b >> 3));
}

void pud_rgb565_to_rgb888(uint16_t px, uint8_t *r, uint8_t *g, uint8_t *b)
{
	uint16_t r5 = (px >> 11) & 0x1Fu;
	uint16_t g6 = (px >> 5) & 0x3Fu;
	uint16_t b5 = px & 0x1Fu;

	*r = (uint8_t)((r5 * 255u + 15u) / 31u);
	*g = (uint8_t)((g6 * 255u + 31u) / 63u);
	*b = (uint8_t)((b5 * 255u + 15u) / 31u);
}

/*
 * Nearest-neighbour scale of an RGBA8888 image (what stb hands us) straight
 * into RGB565.  A null/zero destination size keeps the source size.
 */
uint16_t *pud_rgba_to_rgb565_scaled(const uint8_t *rgba, int sw, int sh, int dw,
                                    int dh)
{
	uint16_t *dst;
	int x, y;

	if (rgba == NULL || sw <= 0 || sh <= 0)
		return NULL;
	if (dw <= 0)
		dw = sw;
	if (dh <= 0)
		dh = sh;

	dst = malloc((size_t)dw * (size_t)dh * sizeof(*dst));
	if (dst == NULL)
		return NULL;

	for (y = 0; y < dh; y++) {
		int sy = y * sh / dh;

		for (x = 0; x < dw; x++) {
			int sx = x * sw / dw;
			const uint8_t *px = &rgba[((size_t)sy * sw + sx) * 4u];

			dst[(size_t)y * dw + x] =
			        pud_rgb888_to_rgb565(px[0], px[1], px[2]);
		}
	}

	return dst;
}

/* Same, for the JPEG path which stays in RGB888. */
uint8_t *pud_rgba_to_rgb888_scaled(const uint8_t *rgba, int sw, int sh, int dw,
                                   int dh)
{
	uint8_t *dst;
	int x, y;

	if (rgba == NULL || sw <= 0 || sh <= 0)
		return NULL;
	if (dw <= 0)
		dw = sw;
	if (dh <= 0)
		dh = sh;

	dst = malloc((size_t)dw * (size_t)dh * 3u);
	if (dst == NULL)
		return NULL;

	for (y = 0; y < dh; y++) {
		int sy = y * sh / dh;

		for (x = 0; x < dw; x++) {
			int sx = x * sw / dw;
			const uint8_t *px = &rgba[((size_t)sy * sw + sx) * 4u];
			uint8_t *out = &dst[((size_t)y * dw + x) * 3u];

			out[0] = px[0];
			out[1] = px[1];
			out[2] = px[2];
		}
	}

	return dst;
}

uint8_t *pud_rgb565_to_rgb888_buf(const uint16_t *px, size_t count)
{
	uint8_t *dst;
	size_t i;

	dst = malloc(count * 3u);
	if (dst == NULL)
		return NULL;

	for (i = 0u; i < count; i++) {
		pud_rgb565_to_rgb888(px[i], &dst[i * 3u], &dst[i * 3u + 1u],
		                     &dst[i * 3u + 2u]);
	}

	return dst;
}

size_t pud_codec_encode_rgb565(int codec, const uint16_t *pixels, size_t count,
                               uint8_t *out, size_t capacity)
{
	if (pixels == NULL || out == NULL || count == 0u)
		return 0u;

	switch (codec) {
	case PUD_CODEC_QOI:
		return rgb565_qoi_compress(pixels, count, out, capacity);
	case PUD_CODEC_RLE:
		return rgb565_rle_compress(pixels, count, out, capacity);
	case PUD_CODEC_LZ4: {
		int written;

		if (count > (size_t)INT32_MAX / 2u ||
		    capacity > (size_t)INT32_MAX)
			return 0u;
		written = LZ4_compress_default((const char *)pixels,
		                               (char *)out, (int)(count * 2u),
		                               (int)capacity);
		return written > 0 ? (size_t)written : 0u;
	}
	default:
		return 0u;
	}
}

size_t pud_codec_decode_rgb565(int codec, const uint8_t *in, size_t size,
                               uint16_t *pixels, size_t capacity)
{
	if (in == NULL || pixels == NULL || size == 0u || capacity == 0u)
		return 0u;

	switch (codec) {
	case PUD_CODEC_QOI:
		return rgb565_qoi_decompress(in, size, pixels, capacity);
	case PUD_CODEC_RLE:
		return rgb565_rle_decompress(in, size, pixels, capacity);
	case PUD_CODEC_LZ4: {
		int written;

		if (size > (size_t)INT32_MAX || capacity > (size_t)INT32_MAX)
			return 0u;
		written = LZ4_decompress_safe((const char *)in, (char *)pixels,
		                              (int)size, (int)(capacity * 2u));
		return written > 0 ? (size_t)written / 2u : 0u;
	}
	default:
		return 0u;
	}
}

/* --------------------------------------------------------------------------
 * JPEG: a growable in-memory sink, because stb has no "how big will it be".
 * -------------------------------------------------------------------------- */

struct jpeg_sink {
	uint8_t *buf;
	size_t len;
	size_t cap;
	int overflow;
};

static void jpeg_sink_write(void *context, void *data, int size)
{
	struct jpeg_sink *sink = context;

	if (size <= 0 || sink->overflow)
		return;
	if (sink->len + (size_t)size > sink->cap) {
		sink->overflow = 1;
		return;
	}
	memcpy(sink->buf + sink->len, data, (size_t)size);
	sink->len += (size_t)size;
}

size_t pud_jpeg_encode(const uint8_t *rgb888, int width, int height,
                       int quality, uint8_t *out, size_t capacity)
{
	struct jpeg_sink sink;
	int ok;

	if (rgb888 == NULL || out == NULL || width <= 0 || height <= 0)
		return 0u;
	if (quality < 1)
		quality = 1;
	if (quality > 100)
		quality = 100;

	sink.buf = out;
	sink.len = 0u;
	sink.cap = capacity;
	sink.overflow = 0;

	ok = stbi_write_jpg_to_func(jpeg_sink_write, &sink, width, height, 3,
	                            rgb888, quality);
	if (!ok || sink.overflow)
		return 0u;

	return sink.len;
}

uint8_t *pud_jpeg_decode(const uint8_t *in, size_t size, int *width,
                         int *height)
{
	uint8_t *decoded;
	uint8_t *copy;
	int w = 0, h = 0, channels = 0;

	if (in == NULL || size == 0u)
		return NULL;

	decoded = stbi_load_from_memory(in, (int)size, &w, &h, &channels, 3);
	if (decoded == NULL) {
		fprintf(stderr, "pudcodec: jpeg decode failed: %s\n",
		        stbi_failure_reason());
		return NULL;
	}

	/* stb owns `decoded`; hand back a plain malloc'd copy so the caller has
	 * one free() contract for every codec. */
	copy = malloc((size_t)w * (size_t)h * 3u);
	if (copy != NULL)
		memcpy(copy, decoded, (size_t)w * (size_t)h * 3u);
	stbi_image_free(decoded);

	if (copy != NULL) {
		if (width != NULL)
			*width = w;
		if (height != NULL)
			*height = h;
	}

	return copy;
}

uint8_t *pud_load_image(const char *path, int *width, int *height)
{
	int channels = 0;

	return stbi_load(path, width, height, &channels, 4);
}

int pud_write_image(const char *path, const uint8_t *rgb888, int width,
                    int height, int quality)
{
	const char *dot = strrchr(path, '.');
	const char *ext = dot != NULL ? dot + 1 : "";
	int ok;

	if (strcasecmp(ext, "png") == 0)
		ok = stbi_write_png(path, width, height, 3, rgb888, width * 3);
	else if (strcasecmp(ext, "bmp") == 0)
		ok = stbi_write_bmp(path, width, height, 3, rgb888);
	else if (strcasecmp(ext, "tga") == 0)
		ok = stbi_write_tga(path, width, height, 3, rgb888);
	else if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0)
		ok = stbi_write_jpg(path, width, height, 3, rgb888,
		                    quality > 0 ? quality : 95);
	else {
		fprintf(stderr,
		        "pudcodec: unknown image extension '.%s' "
		        "(use png, bmp, tga or jpg)\n",
		        ext);
		return -1;
	}

	if (!ok) {
		fprintf(stderr, "pudcodec: cannot write '%s'\n", path);
		return -1;
	}

	return 0;
}
