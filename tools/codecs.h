/*
 * Codec dispatch for the host-side conversion tool.
 *
 * One entry point per direction, with the codec chosen at run time.  The
 * RGB565 codecs call the same library the firmware links, so a stream written
 * here is byte-for-byte what the device decodes.  JPEG is the odd one out: the
 * device decodes JPEG itself, so "encoding" it is just writing a JPEG image and
 * "decoding" it is what stb_image does.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PUD_CODECS_H
#define PUD_CODECS_H

#include <stddef.h>
#include <stdint.h>

enum pud_codec {
	PUD_CODEC_QOI = 0,
	PUD_CODEC_RLE,
	PUD_CODEC_LZ4,
	PUD_CODEC_JPEG,
	PUD_CODEC_COUNT,
};

/* "qoi" -> PUD_CODEC_QOI, ...; -1 for an unknown name. */
int pud_codec_by_name(const char *name);
const char *pud_codec_name(int codec);

/* The DECODER_TYPE the device reports through PUD_CMD_GET_CAPS.  Both JPEG
 * decoders (0 and 1) take the same stream, so JPEG reports 1 (JPEGDEC). */
int pud_codec_decoder_type(int codec);

/* Worst-case encoded size for a width x height RGB565 image, 0 on overflow. */
size_t pud_codec_max_size(int codec, int width, int height);

/* ---- RGB565 codecs (qoi / rle / lz4) ------------------------------------ */

/* Returns the stream size, 0 on failure. */
size_t pud_codec_encode_rgb565(int codec, const uint16_t *pixels, size_t count,
                               uint8_t *out, size_t capacity);

/* Returns the number of pixels written, 0 on failure. */
size_t pud_codec_decode_rgb565(int codec, const uint8_t *in, size_t size,
                               uint16_t *pixels, size_t capacity);

/* ---- JPEG ---------------------------------------------------------------- */

/* `quality` is 1..100.  Returns the JPEG size, 0 on failure. */
size_t pud_jpeg_encode(const uint8_t *rgb888, int width, int height,
                       int quality, uint8_t *out, size_t capacity);

/* Returns a malloc'd RGB888 image, NULL on failure.  `width`/`height` are
 * filled in from the stream. */
uint8_t *pud_jpeg_decode(const uint8_t *in, size_t size, int *width,
                         int *height);

/* ---- pixels -------------------------------------------------------------- */

uint16_t pud_rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b);
void pud_rgb565_to_rgb888(uint16_t px, uint8_t *r, uint8_t *g, uint8_t *b);

/* Nearest-neighbour scale of an RGBA8888 image (what stb_image hands us).
 * dw/dh <= 0 keeps the source size.  Caller frees. */
uint16_t *pud_rgba_to_rgb565_scaled(const uint8_t *rgba, int sw, int sh, int dw,
                                    int dh);
uint8_t *pud_rgba_to_rgb888_scaled(const uint8_t *rgba, int sw, int sh, int dw,
                                   int dh);

/* Expand RGB565 to a malloc'd RGB888 image.  Caller frees. */
uint8_t *pud_rgb565_to_rgb888_buf(const uint16_t *pixels, size_t count);

/* ---- image files (stb_image / stb_image_write live in codecs.c) ---------- */

/* Load any image stb understands as RGBA8888.  Caller frees. */
uint8_t *pud_load_image(const char *path, int *width, int *height);

/* Write RGB888 as PNG/BMP/TGA/JPEG, picked from the path's extension.
 * `quality` is only used for JPEG.  Returns 0 on success. */
int pud_write_image(const char *path, const uint8_t *rgb888, int width,
                    int height, int quality);

#endif /* PUD_CODECS_H */
