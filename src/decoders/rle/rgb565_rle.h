/*
 * RGB565 RLE Compression Library
 *
 * A platform-independent C library for compressing and decompressing
 * RGB565 pixel data using Run-Length Encoding (RLE).
 *
 * This library is designed to work in freestanding environments,
 * including bare-metal MCUs, MPUs, Linux userspace, and kernel space.
 * It depends only on freestanding C99 headers: <stddef.h> and <stdint.h>.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef RGB565_RLE_H
#define RGB565_RLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Compressed Data Format
 *---------------------------------------------------------------------------
 *
 * The compressed stream consists of a 4-byte header followed by a sequence
 * of control + data blocks:
 *
 *   Header: uint32_t pixel_count (little-endian)
 *   Body:   repeated [control_byte][pixel_data...]
 *
 * Control byte encoding:
 *   Bit 7:     0 = literal run, 1 = repeat run
 *   Bits 6..0: run_length - 1  (0 represents 1 pixel, 127 represents 128)
 *
 * A literal run contains 'length' distinct pixels, each stored as 2 bytes
 * (little-endian RGB565).
 *
 * A repeat run contains one pixel value (2 bytes, little-endian) that
 * repeats 'length' times.
 *
 * All multi-byte values are stored in little-endian byte order, making
 * the compressed format portable across platforms regardless of native
 * endianness.
 */

/*---------------------------------------------------------------------------
 * Run-length limits
 *--------------------------------------------------------------------------- */

/** Maximum number of pixels in a single RLE run. */
#define RGB565_RLE_MAX_RUN_LENGTH 128u

/*---------------------------------------------------------------------------
 * API
 *--------------------------------------------------------------------------- */

/**
 * Calculate the worst-case compressed buffer size.
 *
 * In the worst case (e.g., alternating pixel values A, B, A, B...),
 * each pixel requires its own literal run of length 1:
 *   1 control byte + 2 data bytes = 3 bytes per pixel.
 *
 * Total worst-case: 4 (header) + pixel_count * 3.
 *
 * @param pixel_count  Number of RGB565 pixels to compress.
 * @return  Maximum possible compressed size in bytes,
 *          or 0 if pixel_count is 0 or would cause integer overflow.
 */
size_t rgb565_rle_max_compressed_size(size_t pixel_count);

/**
 * Compress an array of RGB565 pixels.
 *
 * @param pixels           Input pixel array (uint16_t per pixel, RGB565).
 * @param pixel_count      Number of pixels in the input.
 * @param output           Output buffer for compressed data.
 * @param output_capacity  Size of the output buffer in bytes.
 * @return  Number of bytes written to output on success,
 *          or 0 on error (NULL pointer, zero count, insufficient capacity).
 */
size_t rgb565_rle_compress(const uint16_t *pixels,
                           size_t pixel_count,
                           uint8_t *output,
                           size_t output_capacity);

/**
 * Decompress an RLE-compressed stream back to RGB565 pixels.
 *
 * @param input           Input buffer containing compressed data.
 * @param input_size      Size of the input buffer in bytes.
 * @param pixels          Output buffer for decompressed pixels.
 * @param pixel_capacity  Maximum number of pixels the output can hold.
 * @return  Number of pixels written on success,
 *          or 0 on error (NULL pointer, malformed data, insufficient capacity,
 *          truncated input).
 */
size_t rgb565_rle_decompress(const uint8_t *input,
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
typedef void (*rgb565_rle_callback)(const uint16_t *pixels,
                                    size_t count,
                                    uint16_t xs, uint16_t ys,
                                    uint16_t xe, uint16_t ye,
                                    void *user_data);

/**
 * Decompress an RLE stream, delivering pixels via a callback.
 *
 * This function is designed for memory-constrained systems (e.g., MCUs)
 * where allocating a full frame buffer is not feasible.  The caller
 * provides one or two accumulation buffers; @p buf_capacity controls
 * the batch granularity independently of RLE run boundaries.
 *
 * When @p buf_b is NULL the library operates in single-buffer mode.
 * When @p buf_b is non-NULL the library ping-pongs between @p buf_a
 * and @p buf_b — while one buffer is being consumed by the callback
 * (e.g. DMA to a display) the library fills the other.
 *
 * The library fills the active buffer with decoded pixels and invokes
 * @p callback when the buffer is full.  Consecutive RLE runs are
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
size_t rgb565_rle_decompress_callback(const uint8_t *input,
                                      size_t input_size,
                                      uint16_t width,
                                      uint16_t *buf_a,
                                      uint16_t *buf_b,
                                      size_t buf_capacity,
                                      rgb565_rle_callback callback,
                                      void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* RGB565_RLE_H */
