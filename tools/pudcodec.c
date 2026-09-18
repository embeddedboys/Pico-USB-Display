/*
 * pudcodec — convert images and frame sequences to and from the streams the
 * Pico USB Display accepts, with the codec picked at run time.
 *
 * The RGB565 codecs (qoi, rle, lz4) link the same library sources the firmware
 * does, so a stream written here is exactly what the device decodes; see
 * notes/decoders.md for what each of them costs on the wire and on the panel.
 * jpeg is a device-side decoder, so its "stream" is just a JPEG image.
 *
 * Ported from the img2x / video2x / x2img tools of the rgb565-rle and
 * rgb565-qoi repositories (MIT): those were three near-identical programs per
 * codec, differing only in which library they called.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec_utils.h"
#include "codecs.h"

#define DEFAULT_JPEG_QUALITY 95
#define MAX_FRAMES 4096

/*
 * The device decodes exactly one LZ4 band per transfer: an LZ4 block cannot be
 * decoded in pieces (every match points back at output the same block produced
 * earlier), so the whole block has to land in one buffer.  The band limit is
 * therefore a device-side buffer, and it is the same one the driver already
 * bands by -- `band_pixels` from PUD_CMD_GET_CAPS:
 *
 *   (65535 - 16) / 3 pixels   ->   * 2 bytes  =  43678 bytes
 *
 * A whole 480x320 frame is 307200 bytes, so anything but a tiny image has to be
 * banded; `img2s`/`video2s` do that for LZ4 and emit a block container.
 */
#define LZ4_BAND_RAW_MAX (2 * ((65535 - 16) / 3))

/* One encoded block: a whole image for every codec but LZ4, one band for LZ4. */
struct blocks {
	uint8_t *data[MAX_FRAMES];
	size_t size[MAX_FRAMES];
	int count;
};

enum mode {
	MODE_NONE = 0,
	MODE_IMG2S,
	MODE_S2IMG,
	MODE_VIDEO2S,
};

struct options {
	int mode;
	int codec;
	const char *input;           /* img2s / s2img */
	const char *output;
	const char *array_name;
	const char *raw_input;       /* video2s --raw */
	const char *t_value;         /* -t, validated per mode */
	const char *list[MAX_FRAMES];
	int list_count;
	int width;                   /* resize, or stream dimensions for s2img */
	int height;
	int quality;
	int band;                    /* LZ4: rows per block (0 = as many as fit) */
};

static void usage(FILE *out, const char *prog)
{
	fprintf(out,
		"Usage:\n"
		"  %s --codec <qoi|rle|lz4|jpeg> img2s   [options] <image>\n"
		"  %s --codec <qoi|rle|lz4|jpeg> s2img   [options] <stream>\n"
		"  %s --codec <qoi|rle|lz4|jpeg> video2s [options] <frames...>\n"
		"\n"
		"Convert to and from the RGB565 streams the Pico USB Display\n"
		"decodes.  The codec is chosen at run time and has to match the\n"
		"DECODER_TYPE the device is built with (jpeg covers 0 and 1).\n"
		"\n"
		"Modes:\n"
		"  img2s    one image  -> one codec stream\n"
		"  s2img    one stream -> one image\n"
		"  video2s  frames     -> one frame container (images or --raw)\n"
		"\n"
		"Options:\n"
		"  --codec <name>  qoi | rle | lz4 | jpeg; 'auto' takes it from a\n"
		"                  .h input that carries a _CODEC tag\n"
		"  -o <file>       output file (default: <input>.<codec>.h/.bin,\n"
		"                  or <input>.png for s2img)\n"
		"  -t <type>       img2s/video2s: 'h' (C header, default) or 'bin'\n"
		"                  s2img: png | jpg | bmp | tga (default png)\n"
		"  -n <name>       C array / base name (default: from the file name)\n"
		"  -w <pixels>     width: resize for img2s/video2s, required for a\n"
		"                  .bin stream in s2img\n"
		"  -h <pixels>     height, same rules as -w\n"
		"  -q <1..100>     JPEG quality (default %d)\n"
		"  --band <rows>   LZ4 only: rows per block (default: as many as the\n"
		"                  device's band buffer takes).  LZ4 is stored as a\n"
		"                  block container, one block per band\n"
		"  --raw <file>    video2s: one file of concatenated raw RGB565\n"
		"                  frames instead of an image list (needs -w/-h)\n"
		"  --help          this text\n"
		"\n"
		"Examples:\n"
		"  %s --codec rle img2s logo.jpg -o bootlogo.rle.h -n bootlogo\n"
		"  %s --codec qoi img2s photo.png -w 480 -h 320 -t bin\n"
		"  %s --codec rle s2img logo.rle.bin -w 480 -h 320\n"
		"  %s --codec qoi video2s frames/*.png -w 480 -h 320 -o clip.qoi.h\n",
		prog, prog, prog, DEFAULT_JPEG_QUALITY, prog, prog, prog, prog);
}

static int parse_args(int argc, char **argv, struct options *opt)
{
	int i;

	memset(opt, 0, sizeof(*opt));
	opt->codec = -1;
	opt->quality = DEFAULT_JPEG_QUALITY;

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (strcmp(arg, "--help") == 0) {
			usage(stdout, argv[0]);
			exit(EXIT_SUCCESS);
		} else if (strcmp(arg, "--codec") == 0 && i + 1 < argc) {
			const char *name = argv[++i];

			if (strcmp(name, "auto") != 0) {
				opt->codec = pud_codec_by_name(name);
				if (opt->codec < 0) {
					fprintf(stderr,
						"pudcodec: unknown codec '%s' "
						"(qoi, rle, lz4, jpeg)\n",
						name);
					return -1;
				}
			}
		} else if (strcmp(arg, "-o") == 0 && i + 1 < argc) {
			opt->output = argv[++i];
		} else if (strcmp(arg, "-n") == 0 && i + 1 < argc) {
			opt->array_name = argv[++i];
		} else if (strcmp(arg, "-w") == 0 && i + 1 < argc) {
			opt->width = atoi(argv[++i]);
		} else if (strcmp(arg, "-h") == 0 && i + 1 < argc) {
			opt->height = atoi(argv[++i]);
		} else if (strcmp(arg, "-q") == 0 && i + 1 < argc) {
			opt->quality = atoi(argv[++i]);
		} else if (strcmp(arg, "--band") == 0 && i + 1 < argc) {
			opt->band = atoi(argv[++i]);
		} else if (strcmp(arg, "--raw") == 0 && i + 1 < argc) {
			opt->raw_input = argv[++i];
		} else if (strcmp(arg, "-t") == 0 && i + 1 < argc) {
			opt->t_value = argv[++i];
		} else if (arg[0] == '-' && arg[1] != '\0') {
			fprintf(stderr, "pudcodec: unknown option '%s'\n\n", arg);
			usage(stderr, argv[0]);
			return -1;
		} else if (opt->mode == MODE_NONE &&
			   (strcmp(arg, "img2s") == 0 ||
			    strcmp(arg, "s2img") == 0 ||
			    strcmp(arg, "video2s") == 0)) {
			opt->mode = strcmp(arg, "img2s") == 0   ? MODE_IMG2S
				    : strcmp(arg, "s2img") == 0 ? MODE_S2IMG
								: MODE_VIDEO2S;
		} else if (opt->mode == MODE_VIDEO2S) {
			if (opt->list_count >= MAX_FRAMES) {
				fprintf(stderr, "pudcodec: too many frames\n");
				return -1;
			}
			opt->list[opt->list_count++] = arg;
		} else {
			opt->input = arg;
		}
	}

	if (opt->mode == MODE_NONE) {
		fprintf(stderr, "pudcodec: no mode given\n\n");
		usage(stderr, argv[0]);
		return -1;
	}

	if (opt->t_value != NULL && opt->mode != MODE_S2IMG &&
	    strcmp(opt->t_value, "h") != 0 && strcmp(opt->t_value, "bin") != 0) {
		fprintf(stderr,
			"pudcodec: -t %s is not valid here "
			"(img2s/video2s take 'h' or 'bin')\n",
			opt->t_value);
		return -1;
	}
	if (opt->t_value != NULL && opt->mode == MODE_S2IMG &&
	    (strcmp(opt->t_value, "h") == 0 || strcmp(opt->t_value, "bin") == 0)) {
		fprintf(stderr,
			"pudcodec: -t %s is not valid here "
			"(s2img takes png, jpg, bmp or tga)\n",
			opt->t_value);
		return -1;
	}

	return 0;
}

static int want_header(const struct options *opt)
{
	return opt->t_value == NULL || strcmp(opt->t_value, "h") == 0;
}

static const char *image_format(const struct options *opt)
{
	return opt->t_value != NULL ? opt->t_value : "png";
}

/* A header written by this tool names its codec, so s2img can pick it up. */
static int codec_from_header(struct options *opt, const char *header_text)
{
	char tag[32];

	if (opt->codec >= 0)
		return 0;

	if (pud_parse_define_str(header_text, "_CODEC", tag, sizeof(tag)) == 0) {
		fprintf(stderr,
			"pudcodec: pass --codec <qoi|rle|lz4|jpeg> "
			"(or --codec auto for a header that names one)\n");
		return -1;
	}

	opt->codec = pud_codec_by_name(tag);
	if (opt->codec < 0) {
		fprintf(stderr, "pudcodec: header asks for unknown codec '%s'\n",
			tag);
		return -1;
	}

	printf("  codec       : %s (from the header)\n",
	       pud_codec_name(opt->codec));
	return 0;
}

/* Encode one RGBA source image (what stb hands us) into a fresh stream. */
static uint8_t *encode_frame(const struct options *opt, const uint8_t *rgba,
			     int src_w, int src_h, int dst_w, int dst_h,
			     size_t *out_size)
{
	size_t capacity = pud_codec_max_size(opt->codec, dst_w, dst_h);
	size_t raw_size = (size_t)dst_w * (size_t)dst_h * 2u;
	uint8_t *out;

	if (capacity == 0u) {
		fprintf(stderr, "pudcodec: bad dimensions %dx%d\n", dst_w, dst_h);
		return NULL;
	}

	out = malloc(capacity);
	if (out == NULL)
		return NULL;

	if (opt->codec == PUD_CODEC_JPEG) {
		uint8_t *rgb = pud_rgba_to_rgb888_scaled(rgba, src_w, src_h,
							 dst_w, dst_h);

		if (rgb == NULL) {
			free(out);
			return NULL;
		}
		*out_size = pud_jpeg_encode(rgb, dst_w, dst_h, opt->quality, out,
					    capacity);
		free(rgb);
	} else {
		uint16_t *px = pud_rgba_to_rgb565_scaled(rgba, src_w, src_h,
							 dst_w, dst_h);

		if (px == NULL) {
			free(out);
			return NULL;
		}
		*out_size = pud_codec_encode_rgb565(opt->codec, px, raw_size / 2u,
						    out, capacity);
		free(px);
	}

	if (*out_size == 0u) {
		fprintf(stderr, "pudcodec: %s encode failed\n",
			pud_codec_name(opt->codec));
		free(out);
		return NULL;
	}

	return out;
}

/*
 * LZ4 band height.  `--band` wins; otherwise take the largest height that fits
 * the device's band buffer *and divides the image*, because the band height is
 * derived from the block count and the image height on the way back (in
 * `s2img` and in the firmware's boot-logo walk), so uniform bands are the ones
 * that stay readable.
 */
static int lz4_band_rows(const struct options *opt, int width, int height)
{
	int max_rows = (int)(LZ4_BAND_RAW_MAX / (2 * (size_t)width));
	int rows;

	if (opt->band > 0)
		return opt->band;
	if (max_rows < 1)
		max_rows = 1;

	for (rows = max_rows; rows > 1; rows--) {
		if (height % rows == 0)
			return rows;
	}

	return max_rows;
}

/*
 * Encode one source image into the blocks the device consumes: a whole image
 * for qoi/rle/jpeg, one band per block for LZ4 (`--band` rows, by default as
 * many rows as the device's band buffer takes).
 */
static int encode_blocks(const struct options *opt, const uint8_t *rgba,
			 int src_w, int src_h, int dst_w, int dst_h,
			 struct blocks *out)
{
	int y, rows;
	size_t capacity;

	memset(out, 0, sizeof(*out));

	if (opt->codec != PUD_CODEC_LZ4) {
		out->data[0] = encode_frame(opt, rgba, src_w, src_h, dst_w, dst_h,
					    &out->size[0]);
		if (out->data[0] == NULL)
			return -1;
		out->count = 1;
		return 0;
	}

	{
		uint16_t *px = pud_rgba_to_rgb565_scaled(rgba, src_w, src_h,
							 dst_w, dst_h);

		if (px == NULL)
			return -1;

		rows = lz4_band_rows(opt, dst_w, dst_h);

		capacity = pud_codec_max_size(opt->codec, dst_w, rows);
		for (y = 0; y < dst_h; y += rows) {
			int bh = dst_h - y < rows ? dst_h - y : rows;
			uint8_t *buf;

			if (out->count >= MAX_FRAMES) {
				free(px);
				return -1;
			}
			buf = malloc(capacity);
			if (buf == NULL) {
				free(px);
				return -1;
			}
			out->size[out->count] = pud_codec_encode_rgb565(
				opt->codec, px + (size_t)y * dst_w,
				(size_t)bh * dst_w, buf, capacity);
			if (out->size[out->count] == 0u) {
				fprintf(stderr, "pudcodec: band at row %d failed\n", y);
				free(buf);
				free(px);
				return -1;
			}
			out->data[out->count++] = buf;
		}
		free(px);
	}

	return 0;
}

static void blocks_free(struct blocks *b)
{
	int i;

	for (i = 0; i < b->count; i++)
		free(b->data[i]);
	b->count = 0;
}

static size_t blocks_total(const struct blocks *b)
{
	size_t total = 0u;
	int i;

	for (i = 0; i < b->count; i++)
		total += b->size[i];

	return total;
}

/* [u32 count][u32 offsets[count+1]][blocks...], in memory.  The same layout
 * pud_write_video_binary() writes, and the same one the firmware walks for the
 * LZ4 boot logo. */
static uint8_t *container_serialize(const struct blocks *b, size_t *size)
{
	size_t header = 4u + 4u * (size_t)(b->count + 1);
	size_t total = header + blocks_total(b);
	uint32_t count = (uint32_t)b->count;
	uint32_t offset = (uint32_t)header;
	uint8_t *buf = malloc(total);
	uint8_t *p;
	int i;

	if (buf == NULL)
		return NULL;

	memcpy(buf, &count, sizeof count);
	memcpy(buf + 4, &offset, sizeof offset);
	for (i = 0; i < b->count; i++) {
		offset += (uint32_t)b->size[i];
		memcpy(buf + 8u + 4u * (size_t)i, &offset, sizeof offset);
	}

	p = buf + header;
	for (i = 0; i < b->count; i++) {
		memcpy(p, b->data[i], b->size[i]);
		p += b->size[i];
	}

	*size = total;
	return buf;
}

static int mode_img2s(struct options *opt)
{
	uint8_t *rgba;
	uint8_t *stream;
	uint8_t *container = NULL;
	struct blocks blocks;
	size_t stream_size = 0u;
	size_t container_size = 0u;
	size_t raw_size;
	int img_w = 0, img_h = 0;
	int out_w, out_h;
	int band_count = 0;
	char path_buf[512];
	char name_buf[256];
	const char *output = opt->output;
	const char *array_name = opt->array_name;
	FILE *fp = NULL;
	int ret = EXIT_FAILURE;

	if (opt->input == NULL) {
		fprintf(stderr, "pudcodec: img2s needs an input image\n");
		return EXIT_FAILURE;
	}
	if (opt->codec < 0) {
		fprintf(stderr, "pudcodec: pass --codec <qoi|rle|lz4|jpeg>\n");
		return EXIT_FAILURE;
	}

	rgba = pud_load_image(opt->input, &img_w, &img_h);
	if (rgba == NULL) {
		fprintf(stderr, "pudcodec: cannot load '%s'\n", opt->input);
		return EXIT_FAILURE;
	}

	out_w = opt->width > 0 ? opt->width : img_w;
	out_h = opt->height > 0 ? opt->height : img_h;

	if (encode_blocks(opt, rgba, img_w, img_h, out_w, out_h, &blocks) != 0) {
		free(rgba);
		return EXIT_FAILURE;
	}
	band_count = blocks.count;
	free(rgba);

	stream_size = blocks_total(&blocks);
	stream = blocks.data[0];
	raw_size = (size_t)out_w * (size_t)out_h * 2u;

	if (output == NULL) {
		char ext[64];

		snprintf(ext, sizeof(ext), ".%s.%s", pud_codec_name(opt->codec),
			 want_header(opt) ? "h" : "bin");
		pud_output_name(opt->input, ext, path_buf, sizeof(path_buf));
		output = path_buf;
	}
	if (array_name == NULL) {
		pud_array_name(output, name_buf, sizeof(name_buf));
		array_name = name_buf;
	}

	fp = fopen(output, "wb");
	if (fp == NULL) {
		fprintf(stderr, "pudcodec: cannot write '%s'\n", output);
		goto done;
	}

	if (opt->codec == PUD_CODEC_LZ4) {
		/* Always a block container, even for a single band: that is the
		 * one shape the device walks (see decoder_draw_bootlogo()), and
		 * what `s2img` reads back. */
		container = container_serialize(&blocks, &container_size);
		if (container == NULL) {
			fprintf(stderr, "pudcodec: out of memory\n");
			goto done;
		}
		stream_size = container_size;
		if (want_header(opt)) {
			if (pud_write_codec_header(fp, pud_codec_name(opt->codec),
						   array_name, opt->input, out_w,
						   out_h, container,
						   container_size, raw_size) != 0) {
				fprintf(stderr, "pudcodec: header write failed\n");
				goto done;
			}
		} else if (pud_write_binary(fp, container,
					    container_size) != 0) {
			fprintf(stderr, "pudcodec: container write failed\n");
			goto done;
		}
	} else if (want_header(opt)) {
		if (pud_write_codec_header(fp, pud_codec_name(opt->codec),
					   array_name, opt->input, out_w, out_h,
					   stream, stream_size, raw_size) != 0) {
			fprintf(stderr, "pudcodec: header write failed\n");
			goto done;
		}
	} else if (pud_write_binary(fp, stream, stream_size) != 0) {
		fprintf(stderr, "pudcodec: binary write failed\n");
		goto done;
	}

	ret = EXIT_SUCCESS;

done:
	if (fp != NULL)
		fclose(fp);
	blocks_free(&blocks);
	free(container);

	printf("\n");
	pud_print_size_comparison(pud_basename(opt->input), raw_size,
				  stream_size);
	printf("  dimensions  : %d x %d (%zu pixels, RGB565)\n", out_w, out_h,
	       (size_t)out_w * (size_t)out_h);
	if (opt->codec == PUD_CODEC_LZ4)
		printf("  bands       : %d block(s), rows of %d\n",
		       band_count, lz4_band_rows(opt, out_w, out_h));
	printf("  decoder type: %d (PUD_CMD_GET_CAPS)\n",
	       pud_codec_decoder_type(opt->codec));
	printf("  output      : %s (%s)\n\n", output,
	       want_header(opt) ? "C header" : "binary");

	return ret;
}

/*
 * Load a stream and its dimensions from either a generated C header or a raw
 * binary.  A header carries the codec, the dimensions and the length; a raw
 * binary carries nothing, so it needs -w and -h -- except for JPEG, whose
 * dimensions come from the image itself.
 */
static uint8_t *load_stream(const struct options *opt, const char *header_text,
			    size_t header_size, size_t *stream_size, int *width,
			    int *height, int self_sized)
{
	uint8_t *stream;
	long declared;

	if (header_text == NULL) {
		stream = pud_read_file(opt->input, stream_size);
		if (stream == NULL)
			return NULL;

		*width = opt->width;
		*height = opt->height;
		if (!self_sized && (*width <= 0 || *height <= 0)) {
			fprintf(stderr,
				"pudcodec: this stream carries no dimensions, "
				"so it needs -w and -h\n");
			free(stream);
			return NULL;
		}

		return stream;
	}

	*width = (int)pud_parse_define(header_text, "_WIDTH", 0);
	*height = (int)pud_parse_define(header_text, "_HEIGHT", 0);
	if (*width <= 0 || *height <= 0) {
		fprintf(stderr, "pudcodec: '%s' has no dimensions\n",
			opt->input);
		return NULL;
	}

	/* A container header holds every block; block 0 is the one to decode,
	 * and its length is a define of its own. */
	declared = pud_parse_define(header_text, "_BLOCK0_SIZE", -1);
	if (declared > 0)
		printf("  note        : block container, decoding block 0\n");
	else
		declared = pud_parse_define(header_text, "_SIZE", -1);
	if (declared <= 0 || (size_t)declared > header_size)
		declared = (long)header_size;

	stream = malloc((size_t)declared + 1u);
	if (stream == NULL)
		return NULL;

	*stream_size = pud_parse_codec_array(header_text, stream,
					     (size_t)declared);
	if (*stream_size == 0u) {
		fprintf(stderr, "pudcodec: no codec array in '%s'\n", opt->input);
		free(stream);
		return NULL;
	}

	return stream;
}

/*
 * LZ4 streams are block containers: one block per band, because an LZ4 block
 * cannot be decoded in pieces.  Decode them all and put the bands back
 * together into one RGB888 image.  The bands are uniform, so the row height
 * follows from the block count and the image height.
 */
static uint8_t *lz4_decode_container(const uint8_t *blob, size_t size,
				     int width, int height, int count,
				     int *out_w, int *out_h)
{
	uint16_t *frame;
	uint16_t *band;
	int rows, y, i;
	uint32_t offset;

	if (blob == NULL || size < 8u || count <= 0 || width <= 0 ||
	    height <= 0 || height % count != 0)
		return NULL;

	rows = height / count;
	frame = malloc((size_t)width * (size_t)height * sizeof(*frame));
	band = malloc((size_t)width * (size_t)rows * sizeof(*band));
	if (frame == NULL || band == NULL) {
		free(frame);
		free(band);
		return NULL;
	}

	offset = (uint32_t)(4u + 4u * (size_t)(count + 1));
	for (y = 0, i = 0; i < count; i++) {
		uint32_t start, end;
		size_t got;

		if (4u + 4u * (size_t)i + 8u > size)
			break;
		memcpy(&start, blob + 4u + 4u * (size_t)i, 4);
		memcpy(&end, blob + 4u + 4u * (size_t)(i + 1), 4);
		if (end < start || end > size)
			break;

		got = pud_codec_decode_rgb565(PUD_CODEC_LZ4, blob + start,
					      end - start, band,
					      (size_t)width * rows);
		if (got != (size_t)width * (size_t)rows) {
			fprintf(stderr, "pudcodec: lz4 band %d did not decode\n", i);
			free(frame);
			free(band);
			return NULL;
		}
		memcpy(frame + (size_t)y * width, band,
		       (size_t)width * rows * sizeof(*band));
		y += rows;
	}
	(void)offset;
	free(band);

	if (y != height) {
		free(frame);
		return NULL;
	}

	*out_w = width;
	*out_h = height;
	return pud_rgb565_to_rgb888_buf(frame, (size_t)width * height);
}

static int mode_s2img(struct options *opt)
{
	char path_buf[512];
	char *header_text = NULL;
	size_t header_size = 0u;
	uint8_t *stream;
	size_t stream_size = 0u;
	int width = 0, height = 0;
	int out_w = 0, out_h = 0;
	uint16_t *pixels = NULL;
	uint8_t *rgb = NULL;
	const char *output = opt->output;
	int ret = EXIT_FAILURE;

	if (opt->input == NULL) {
		fprintf(stderr, "pudcodec: s2img needs an input stream\n");
		return EXIT_FAILURE;
	}

	if (strstr(opt->input, ".h") != NULL) {
		header_text = (char *)pud_read_file(opt->input, &header_size);
		if (header_text == NULL)
			return EXIT_FAILURE;
		if (codec_from_header(opt, header_text) != 0) {
			free(header_text);
			return EXIT_FAILURE;
		}
	}
	if (opt->codec < 0) {
		fprintf(stderr, "pudcodec: pass --codec <qoi|rle|lz4|jpeg>\n");
		free(header_text);
		return EXIT_FAILURE;
	}

	stream = load_stream(opt, header_text, header_size, &stream_size,
			     &width, &height, opt->codec == PUD_CODEC_JPEG);
	free(header_text);
	if (stream == NULL)
		return EXIT_FAILURE;

	if (opt->codec == PUD_CODEC_LZ4) {
		uint32_t blocks = 0u;

		if (stream_size < 4u) {
			free(stream);
			return EXIT_FAILURE;
		}
		memcpy(&blocks, stream, 4);
		rgb = lz4_decode_container(stream, stream_size, width, height,
					   (int)blocks, &out_w, &out_h);
		if (rgb == NULL) {
			fprintf(stderr,
				"pudcodec: not an LZ4 block container, or the "
				"band geometry does not fit -w/-h\n");
			free(stream);
			return EXIT_FAILURE;
		}
	} else if (opt->codec == PUD_CODEC_JPEG) {
		rgb = pud_jpeg_decode(stream, stream_size, &out_w, &out_h);
		if (rgb == NULL) {
			free(stream);
			return EXIT_FAILURE;
		}
	} else {
		size_t count = (size_t)width * (size_t)height;

		pixels = malloc(count * sizeof(*pixels));
		if (pixels == NULL) {
			free(stream);
			return EXIT_FAILURE;
		}

		count = pud_codec_decode_rgb565(opt->codec, stream, stream_size,
						pixels, count);
		if (count == 0u) {
			fprintf(stderr,
				"pudcodec: %s decode failed "
				"(wrong codec, or wrong -w/-h?)\n",
				pud_codec_name(opt->codec));
			free(pixels);
			free(stream);
			return EXIT_FAILURE;
		}

		out_w = width;
		out_h = height;
		rgb = pud_rgb565_to_rgb888_buf(pixels, count);
		free(pixels);
		if (rgb == NULL) {
			free(stream);
			return EXIT_FAILURE;
		}
	}
	free(stream);

	if (output == NULL) {
		char ext[64];

		snprintf(ext, sizeof(ext), ".%s", image_format(opt));
		pud_output_name(opt->input, ext, path_buf, sizeof(path_buf));
		output = path_buf;
	}

	if (pud_write_image(output, rgb, out_w, out_h, opt->quality) != 0)
		goto done;

	ret = EXIT_SUCCESS;

done:
	free(rgb);

	printf("\n");
	printf("  decoded     : %d x %d (%zu pixels) from %zu encoded bytes\n",
	       out_w, out_h, (size_t)out_w * (size_t)out_h, stream_size);
	if (opt->codec != PUD_CODEC_JPEG)
		pud_print_size_comparison("stream", (size_t)out_w *
							  (size_t)out_h * 2u,
					  stream_size);
	printf("  output      : %s\n\n", output);

	return ret;
}

static int mode_video2s(struct options *opt)
{
	uint8_t *frames[MAX_FRAMES];
	size_t sizes[MAX_FRAMES];
	size_t total_raw = 0u, total_encoded = 0u;
	int count = 0;
	int raw_frames = 0;
	int width = opt->width, height = opt->height;
	char path_buf[512];
	char name_buf[256];
	const char *output = opt->output;
	const char *base_name = opt->array_name;
	FILE *fp = NULL;
	int ret = EXIT_FAILURE;
	int i;

	memset(frames, 0, sizeof(frames));
	memset(sizes, 0, sizeof(sizes));

	if (opt->codec < 0) {
		fprintf(stderr, "pudcodec: pass --codec <qoi|rle|lz4|jpeg>\n");
		return EXIT_FAILURE;
	}

	if (opt->raw_input != NULL) {
		size_t raw_size = 0u;
		uint8_t *raw = pud_read_file(opt->raw_input, &raw_size);
		size_t frame_bytes;

		if (raw == NULL)
			return EXIT_FAILURE;
		if (width <= 0 || height <= 0) {
			fprintf(stderr,
				"pudcodec: --raw needs -w and -h "
				"(the file is raw RGB565)\n");
			free(raw);
			return EXIT_FAILURE;
		}

		frame_bytes = (size_t)width * (size_t)height * 2u;
		if (raw_size == 0u || raw_size % frame_bytes != 0u) {
			fprintf(stderr,
				"pudcodec: %zu bytes is not a whole number of "
				"%dx%d RGB565 frames\n",
				raw_size, width, height);
			free(raw);
			return EXIT_FAILURE;
		}

		raw_frames = (int)(raw_size / frame_bytes);
		if (raw_frames > MAX_FRAMES) {
			fprintf(stderr, "pudcodec: more than %d frames\n",
				MAX_FRAMES);
			free(raw);
			return EXIT_FAILURE;
		}

		for (i = 0; i < raw_frames; i++) {
			const uint16_t *px =
				(const uint16_t *)(raw + (size_t)i * frame_bytes);

			if (opt->codec == PUD_CODEC_LZ4) {
				/* one block per band, like the image path */
				int rows = lz4_band_rows(opt, width, height);
				int y;

				for (y = 0; y < height; y += rows) {
					int bh = height - y < rows ? height - y : rows;
					size_t capacity = pud_codec_max_size(
						opt->codec, width, bh);
					uint8_t *out = malloc(capacity);
					size_t got;

					if (out == NULL || count >= MAX_FRAMES) {
						free(out);
						free(raw);
						goto done;
					}
					got = pud_codec_encode_rgb565(
						opt->codec,
						px + (size_t)y * width,
						(size_t)bh * width, out, capacity);
					if (got == 0u) {
						fprintf(stderr,
							"pudcodec: frame %d band %d failed\n",
							i, y / rows);
						free(out);
						free(raw);
						goto done;
					}
					frames[count] = out;
					sizes[count] = got;
					count++;
					total_encoded += got;
				}
				total_raw += frame_bytes;
				continue;
			}

			{
				size_t capacity = pud_codec_max_size(opt->codec,
								     width,
								     height);
				uint8_t *out = malloc(capacity);

				if (out == NULL) {
					free(raw);
					goto done;
				}
				sizes[count] = pud_codec_encode_rgb565(
					opt->codec, px, (size_t)width * height,
					out, capacity);
				if (sizes[count] == 0u) {
					fprintf(stderr,
						"pudcodec: frame %d failed\n", i);
					free(out);
					free(raw);
					goto done;
				}
				frames[count] = out;
				count++;
				total_raw += frame_bytes;
				total_encoded += sizes[count - 1];
			}
		}
		free(raw);
	} else {
		for (i = 0; i < opt->list_count; i++) {
			int img_w = 0, img_h = 0;
			struct blocks fb;
			uint8_t *rgba =
				pud_load_image(opt->list[i], &img_w, &img_h);
			int b;

			if (rgba == NULL) {
				fprintf(stderr, "pudcodec: cannot load '%s'\n",
					opt->list[i]);
				goto done;
			}
			if (width <= 0)
				width = img_w;
			if (height <= 0)
				height = img_h;

			if (encode_blocks(opt, rgba, img_w, img_h, width, height,
					  &fb) != 0) {
				free(rgba);
				goto done;
			}
			free(rgba);

			/* LZ4 gives one block per band, so a frame contributes
			 * several; the container stays flat either way. */
			for (b = 0; b < fb.count; b++) {
				if (count >= MAX_FRAMES) {
					blocks_free(&fb);
					goto done;
				}
				frames[count] = fb.data[b];
				sizes[count] = fb.size[b];
				count++;
				total_encoded += fb.size[b];
			}
			total_raw += (size_t)width * (size_t)height * 2u;
		}

		if (count == 0) {
			fprintf(stderr, "pudcodec: video2s needs frames (or --raw)\n");
			return EXIT_FAILURE;
		}
	}

	if (output == NULL) {
		char ext[64];

		snprintf(ext, sizeof(ext), ".%s.%s", pud_codec_name(opt->codec),
			 want_header(opt) ? "h" : "bin");
		pud_output_name(opt->raw_input != NULL ? opt->raw_input
						       : opt->list[0],
				ext, path_buf, sizeof(path_buf));
		output = path_buf;
	}

	/* The C names come from the output file unless -n says otherwise, the
	 * same way img2s derives its array name. */
	if (opt->array_name == NULL) {
		pud_array_name(output, name_buf, sizeof(name_buf));
		base_name = name_buf;
	}

	fp = fopen(output, "wb");
	if (fp == NULL) {
		fprintf(stderr, "pudcodec: cannot write '%s'\n", output);
		goto done;
	}

	if (want_header(opt)) {
		if (pud_write_video_header(fp, pud_codec_name(opt->codec),
					   base_name,
					   opt->codec == PUD_CODEC_LZ4
						   ? "bands (LZ4 blocks)"
						   : "frames",
					   width, height, frames, sizes,
					   count, total_raw) != 0) {
			fprintf(stderr, "pudcodec: header write failed\n");
			goto done;
		}
	} else if (pud_write_video_binary(fp, frames, sizes, count) != 0) {
		fprintf(stderr, "pudcodec: container write failed\n");
		goto done;
	}

	ret = EXIT_SUCCESS;

done:
	if (fp != NULL)
		fclose(fp);
	for (i = 0; i < count; i++)
		free(frames[i]);

	printf("\n");
	printf("  codec       : %s\n", pud_codec_name(opt->codec));
	printf("  frames      : %d at %d x %d\n", count, width, height);
	printf("  raw total   : %zu B, encoded total: %zu B", total_raw,
	       total_encoded);
	if (total_raw > 0u)
		printf(" (%.1f%%)",
		       (double)total_encoded / (double)total_raw * 100.0);
	printf("\n");
	printf("  output      : %s (%s)\n\n", output,
	       want_header(opt) ? "C header" : "binary container");

	return ret;
}

int main(int argc, char **argv)
{
	struct options opt;

	if (parse_args(argc, argv, &opt) != 0)
		return EXIT_FAILURE;

	switch (opt.mode) {
	case MODE_IMG2S:
		return mode_img2s(&opt);
	case MODE_S2IMG:
		return mode_s2img(&opt);
	case MODE_VIDEO2S:
		return mode_video2s(&opt);
	default:
		usage(stderr, argv[0]);
		return EXIT_FAILURE;
	}
}
