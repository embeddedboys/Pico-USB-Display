/*
 * Shared helpers for the host-side conversion tool.
 *
 * Derived from the rgb565-rle / rgb565-qoi `*_utils.c` tools (MIT): the
 * header writer, the size comparison and the C-header parser were per-codec
 * copies there, and are codec-parameterised here so one tool can emit and read
 * all of them.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PUD_CODEC_UTILS_H
#define PUD_CODEC_UTILS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Write a codec stream as a C header: a codec tag, the dimensions, the sizes
 * and the array itself.  `raw_size` is the uncompressed RGB565 size in bytes
 * and is only used for the comment and the ratio.
 *
 * Returns 0 on success.
 */
int pud_write_codec_header(FILE *out, const char *codec, const char *array_name,
                           const char *source_name, int width, int height,
                           const uint8_t *data, size_t size, size_t raw_size);

/* Write a codec stream as a plain binary file.  Returns 0 on success. */
int pud_write_binary(FILE *out, const uint8_t *data, size_t size);

/*
 * Write a block sequence as a C header: the dimensions, the block count, a
 * byte-offset table and the concatenated blocks.  Block 0's size is a define
 * too, so a single block can be taken straight out of the header.
 *
 * A container is a list of blocks: for a video each block is a frame, for LZ4
 * each block is a band (`unit` only names them in the comment).  Returns 0 on
 * success.
 */
int pud_write_video_header(FILE *out, const char *codec, const char *base_name,
                           const char *unit, int width, int height,
                           uint8_t *const *blocks, const size_t *sizes,
                           int count, size_t total_raw);

/*
 * Write a block sequence as the binary container:
 *
 *   [u32 block_count][u32 offsets[block_count + 1]][block data...]
 *
 * with offsets in file order and the last one equal to the file size.
 *
 * Returns 0 on success.
 */
int pud_write_video_binary(FILE *out, uint8_t *const *blocks,
                           const size_t *sizes, int count);

/* Read a whole file into a malloc'd, NUL-terminated buffer.  NULL on error. */
uint8_t *pud_read_file(const char *path, size_t *size);

/* "./a/b/c.png" -> "c.png" */
const char *pud_basename(const char *path);

/* "a/b/boot logo.jpg" -> "boot_logo_jpg" (a valid C identifier) */
void pud_array_name(const char *path, char *out, size_t out_size);

/* "a/b/img.png" + ".qoi.h" -> "a/b/img.qoi.h" */
void pud_output_name(const char *input, const char *ext, char *out,
                     size_t out_size);

/*
 * Header parsing.  `suffix` is matched against the end of the define name, so
 * "_WIDTH" finds IMG_WIDTH, LOGO_WIDTH, ...  Returns `fallback` if absent.
 */
long pud_parse_define(const char *text, const char *suffix, long fallback);

/* Same, for a string define such as _CODEC "qoi".  Returns 0 and fills `out`. */
int pud_parse_define_str(const char *text, const char *suffix, char *out,
                         size_t out_size);

/*
 * Extract the bytes of the last `const uint8_t ...[...] = { ... }` array in a
 * header.  Returns the number of bytes written, or 0 if there is no array.
 */
size_t pud_parse_codec_array(const char *text, uint8_t *out, size_t capacity);

/* "solid 480x320: raw 307200 B -> 3638 B (1.2%)" */
void pud_print_size_comparison(const char *label, size_t raw_bytes,
                               size_t compressed_bytes);

#endif /* PUD_CODEC_UTILS_H */
