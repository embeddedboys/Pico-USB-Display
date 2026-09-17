/*
 * RGB565 QOI Compression Library
 *
 * A platform-independent C library for compressing and decompressing
 * RGB565 pixel data using the Quite OK Image (QOI) algorithm,
 * adapted for 16-bit RGB565 colour format.
 *
 * This library is designed to work in freestanding environments,
 * including bare-metal MCUs, MPUs, Linux userspace, and kernel space.
 * It depends only on freestanding C99 headers: <stddef.h> and <stdint.h>.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef RGB565_QOI_H
#define RGB565_QOI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Compressed Data Format
 *---------------------------------------------------------------------------
 *
 * The compressed stream consists of an 8-byte header, followed by a sequence
 * of QOI-encoded chunks, and an 8-byte end marker:
 *
 *   Header: magic "q565" (4 bytes) + uint32_t pixel_count (little-endian)
 *   Body:   QOI chunks (variable length)
 *   End:    8-byte marker {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01}
 *
 * Chunk types (each identified by the first byte):
 *
 *   QOI_OP_INDEX  0x00..0x3F  (00xxxxxx)
 *     Look up the 6-bit hash index in the running array of 64 previously
 *     seen pixels.  No additional data bytes.
 *
 *   QOI_OP_DIFF   0x40..0x7F  (01xxxxxx)
 *     2-bit signed differences for R, G, B from the previous pixel,
 *     each biased by 2 (range -2..1).  Layout:
 *       bits [5:4] = dr + 2
 *       bits [3:2] = dg + 2
 *       bits [1:0] = db + 2
 *     No additional data bytes.
 *
 *   QOI_OP_LUMA   0x80..0xBF  (10xxxxxx)
 *     6-bit signed green difference (biased by 32, range -32..31) followed
 *     by one byte containing 4-bit red-green and blue-green differences
 *     (each biased by 8, range -8..7).  Layout:
 *       Byte 0 bits [5:0] = dg + 32
 *       Byte 1 bits [7:4] = (dr - dg) + 8
 *       Byte 1 bits [3:0] = (db - dg) + 8
 *
 *   QOI_OP_RUN    0xC0..0xFD  (11xxxxxx, excluding 0xFE and 0xFF)
 *     Run of identical pixels from the previous pixel value.
 *     Run length = (tag & 0x3F) + 1, range 1..62.
 *     No additional data bytes.
 *
 *   QOI_OP_RGB565 0xFE
 *     A full RGB565 pixel follows as 2 bytes (little-endian).
 *     Used when no other encoding is efficient.
 *
 * All multi-byte values are stored in little-endian byte order.
 * Channel arithmetic wraps (modulo 32 for R/B, modulo 64 for G).
 *
 * The hash function for the 64-entry index array:
 *   hash = (r * 3 + g * 5 + b * 7) & 0x3F
 *   where r = (pixel >> 11) & 0x1F  (5-bit red)
 *         g = (pixel >>  5) & 0x3F  (6-bit green)
 *         b =  pixel        & 0x1F  (5-bit blue)
 */

/*---------------------------------------------------------------------------
 * Constants
 *--------------------------------------------------------------------------- */

/** Maximum number of pixels in a single QOI run. */
#define RGB565_QOI_MAX_RUN 62u

/*---------------------------------------------------------------------------
 * API
 *--------------------------------------------------------------------------- */

/**
 * Calculate the worst-case compressed buffer size.
 *
 * In the worst case every pixel must be emitted as QOI_OP_RGB565:
 *   1 tag byte (0xFE) + 2 data bytes = 3 bytes per pixel.
 *
 * Total worst-case: 8 (header) + pixel_count * 3 + 8 (end marker).
 *
 * @param pixel_count  Number of RGB565 pixels to compress.
 * @return  Maximum possible compressed size in bytes,
 *          or 0 if pixel_count is 0 or would cause integer overflow.
 */
size_t rgb565_qoi_max_compressed_size(size_t pixel_count);

/**
 * Compress an array of RGB565 pixels using QOI encoding.
 *
 * @param pixels           Input pixel array (uint16_t per pixel, RGB565).
 * @param pixel_count      Number of pixels in the input.
 * @param output           Output buffer for compressed data.
 * @param output_capacity  Size of the output buffer in bytes.
 * @return  Number of bytes written to output on success,
 *          or 0 on error (NULL pointer, zero count, insufficient capacity).
 */
size_t rgb565_qoi_compress(const uint16_t *pixels,
                           size_t pixel_count,
                           uint8_t *output,
                           size_t output_capacity);

/**
 * Decompress a QOI-encoded stream back to RGB565 pixels.
 *
 * @param input           Input buffer containing compressed data.
 * @param input_size      Size of the input buffer in bytes.
 * @param pixels          Output buffer for decompressed pixels.
 * @param pixel_capacity  Maximum number of pixels the output can hold.
 * @return  Number of pixels written on success,
 *          or 0 on error (NULL pointer, bad magic, malformed data,
 *          insufficient capacity, truncated input).
 */
size_t rgb565_qoi_decompress(const uint8_t *input,
                             size_t input_size,
                             uint16_t *pixels,
                             size_t pixel_capacity);

/**
 * Callback type for streaming decompression.
 *
 * Called when the accumulation buffer is full or all pixels have been
 * decoded.  The coordinates (@p xs, @p ys) to (@p xe, @p ye) define
 * the bounding rectangle of the batch, which may span multiple rows.
 *
 * @p pixels is the caller's accumulation buffer; only the first @p count
 * entries are valid.  The data is in native uint16_t byte order and is
 * valid only for the duration of the callback.
 *
 * @param pixels     Array of @p count pixel values (the caller's buffer).
 * @param count      Number of valid pixels in this batch.
 * @param xs         Starting column (x) of this batch.
 * @param ys         Starting row (y) of this batch.
 * @param xe         Ending column (x) of this batch.
 * @param ye         Ending row (y) of this batch.
 * @param user_data  Opaque pointer passed through from the API.
 */
typedef void (*rgb565_qoi_callback)(const uint16_t *pixels,
                                    size_t count,
                                    uint16_t xs, uint16_t ys,
                                    uint16_t xe, uint16_t ye,
                                    void *user_data);

/**
 * Decompress a QOI stream, delivering pixels via a callback.
 *
 * This function is designed for memory-constrained systems (e.g., MCUs)
 * where allocating a full frame buffer is not feasible.  The caller
 * provides one or two accumulation buffers; @p buf_capacity controls
 * the batch granularity independently of QOI run boundaries.
 *
 * When @p buf_b is NULL the library operates in single-buffer mode.
 * When @p buf_b is non-NULL the library ping-pongs between @p buf_a
 * and @p buf_b — while one buffer is being consumed by the callback
 * (e.g. DMA to a display) the library fills the other.
 *
 * The library fills the active buffer with decoded pixels and invokes
 * @p callback when the buffer is full.  Consecutive QOI runs are
 * automatically merged across row boundaries.  Each callback invocation
 * may span multiple rows; the coordinates (@p xs, @p ys) to
 * (@p xe, @p ye) define the bounding rectangle.
 *
 * @param input        Input buffer containing compressed data.
 * @param input_size   Size of the input buffer in bytes.
 * @param width        Image width in pixels (must be > 0).
 * @param buf_a        First accumulation buffer (must not be NULL).
 * @param buf_b        Second accumulation buffer, or NULL for
 *                     single-buffer mode.
 * @param buf_capacity Capacity of each buffer in pixels (must be > 0).
 * @param callback     Called when the active buffer is full (must not
 *                     be NULL).
 * @param user_data    Opaque pointer passed through to the callback.
 * @return  Total number of pixels decompressed on success,
 *          or 0 on error (NULL pointer, width == 0, buf_capacity == 0,
 *          malformed data, truncated input).
 */
size_t rgb565_qoi_decompress_callback(const uint8_t *input,
                                      size_t input_size,
                                      uint16_t width,
                                      uint16_t *buf_a,
                                      uint16_t *buf_b,
                                      size_t buf_capacity,
                                      rgb565_qoi_callback callback,
                                      void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* RGB565_QOI_H */
