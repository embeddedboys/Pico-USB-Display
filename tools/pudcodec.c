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

static int mode_img2s(struct options *opt)
{
	uint8_t *rgba;
	uint8_t *stream;
	size_t stream_size = 0u;
	size_t raw_size;
	int img_w = 0, img_h = 0;
	int out_w, out_h;
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

	stream = encode_frame(opt, rgba, img_w, img_h, out_w, out_h,
			      &stream_size);
	free(rgba);
	if (stream == NULL)
		return EXIT_FAILURE;

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

	if (want_header(opt)) {
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
	free(stream);

	printf("\n");
	pud_print_size_comparison(pud_basename(opt->input), raw_size,
				  stream_size);
	printf("  dimensions  : %d x %d (%zu pixels, RGB565)\n", out_w, out_h,
	       (size_t)out_w * (size_t)out_h);
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

	/* A video header holds every frame; frame 0 is the one to decode, and
	 * its length is a define of its own. */
	declared = pud_parse_define(header_text, "_FRAME0_SIZE", -1);
	if (declared > 0)
		printf("  note        : video header, decoding frame 0\n");
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

	if (opt->codec == PUD_CODEC_JPEG) {
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

		count = (int)(raw_size / frame_bytes);
		if (count > MAX_FRAMES) {
			fprintf(stderr, "pudcodec: more than %d frames\n",
				MAX_FRAMES);
			free(raw);
			return EXIT_FAILURE;
		}

		for (i = 0; i < count; i++) {
			size_t capacity = pud_codec_max_size(opt->codec, width,
							     height);
			uint8_t *out = malloc(capacity);

			if (out == NULL) {
				free(raw);
				goto done;
			}
			sizes[i] = pud_codec_encode_rgb565(
				opt->codec,
				(const uint16_t *)(raw +
						   (size_t)i * frame_bytes),
				(size_t)width * height, out, capacity);
			if (sizes[i] == 0u) {
				fprintf(stderr, "pudcodec: frame %d failed\n", i);
				free(out);
				free(raw);
				goto done;
			}
			frames[i] = out;
			total_raw += frame_bytes;
			total_encoded += sizes[i];
		}
		free(raw);
	} else {
		for (i = 0; i < opt->list_count; i++) {
			int img_w = 0, img_h = 0;
			uint8_t *rgba =
				pud_load_image(opt->list[i], &img_w, &img_h);
			size_t size = 0u;
			uint8_t *out;

			if (rgba == NULL) {
				fprintf(stderr, "pudcodec: cannot load '%s'\n",
					opt->list[i]);
				goto done;
			}
			if (width <= 0)
				width = img_w;
			if (height <= 0)
				height = img_h;

			out = encode_frame(opt, rgba, img_w, img_h, width, height,
					   &size);
			free(rgba);
			if (out == NULL)
				goto done;

			frames[count] = out;
			sizes[count] = size;
			count++;
			total_raw += (size_t)width * (size_t)height * 2u;
			total_encoded += size;
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
					   base_name, width, height, frames,
					   sizes, count, total_raw) != 0) {
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
