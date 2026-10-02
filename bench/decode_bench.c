/*
 * Decoder throughput, measured.
 *
 * One metric per codec: how fast the QOI / RLE / LZ4 decoders turn a compressed
 * stream back into RGB565.  The image is compressed once at start-up with the
 * matching in-tree encoder and decoded REPS times after that, so the number is
 * the decoder's, not the encoder's.
 *
 * Nothing here is theoretical: every rate printed comes from time_us_32()
 * around the decode loop, and each codec has to reproduce the source image
 * byte for byte before its timing is reported -- a decoder that is fast because
 * it is wrong would otherwise look like a win.
 */
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/uart.h"

#include "rgb565_qoi.h"
#include "rgb565_rle.h"
#include "lz4.h"

/* A band-shaped image: what the device actually decodes in one go is a band,
 * not a whole screen, so measure that unit and scale up at the end. */
#define IMG_W 480
#define IMG_H 64
#define IMG_PIXELS (IMG_W * IMG_H)
#define REPS 32

static uint16_t s_src[IMG_PIXELS];
static uint16_t s_out[IMG_PIXELS];
static uint8_t s_comp[IMG_PIXELS * 3 + 64];
static uint8_t s_work[IMG_PIXELS * 3 + 64];

static uint32_t s_comp_size;

static void report(const char *name, uint32_t us, size_t bytes_in)
{
	uint64_t in_bps = us ? (uint64_t)bytes_in * 1000000u / us : 0;
	uint64_t px_per_s = us ? (uint64_t)IMG_PIXELS * REPS * 1000000u / us : 0;
	/* A full 480x320 screen is 307200 px, i.e. this image times 10. */
	uint32_t screen_us = us ? (uint32_t)(us / REPS) * 10u : 0;

	printf("[bench] %-4s in %6u B  %6u us/band  %4u.%02u MB/s in  %5u.%02u MPix/s  -> full screen %u us\n",
	       name, (unsigned)bytes_in, (unsigned)(us / REPS),
	       (unsigned)(in_bps / 1000000u), (unsigned)((in_bps / 10000u) % 100u),
	       (unsigned)(px_per_s / 1000000u), (unsigned)((px_per_s / 10000u) % 100u),
	       (unsigned)screen_us);
}

int main(void)
{
	uint32_t t0, us;
	size_t i, n;
	int ok;

	stdio_uart_init_full(uart0, DEBUG_UART_SPEED, DEBUG_UART_TX_PIN, DEBUG_UART_RX_PIN);
	printf("\n[bench] decoder throughput\n");
	printf("[bench] image %dx%d (%d px), %d reps per codec\n", IMG_W, IMG_H,
	       IMG_PIXELS, REPS);

	/* Structure the encoders can exploit, like real panel content: runs,
	 * gradients and repeated glyph-sized blocks. */
	for (i = 0; i < IMG_PIXELS; i++) {
		unsigned x = i % IMG_W, y = i / IMG_W;

		s_src[i] = (uint16_t)(((x / 16) ^ (y / 4)) & 1 ? 0xf81f : 0x07e0);
		if ((x % 32) < 3)
			s_src[i] = (uint16_t)((x * 8) & 0x1f);
		if ((y % 8) == 0)
			s_src[i] = 0xffff;
	}

	/* ---- QOI ---- */
	n = rgb565_qoi_compress(s_src, IMG_PIXELS, s_comp, sizeof(s_comp));
	s_comp_size = (uint32_t)n;
	if (!n) {
		printf("[bench] QOI compress failed\n");
	} else {
		memset(s_out, 0, sizeof(s_out));
		ok = rgb565_qoi_decompress(s_comp, n, s_out, IMG_PIXELS) == IMG_PIXELS &&
		     memcmp(s_src, s_out, sizeof(s_src)) == 0;
		t0 = time_us_32();
		for (i = 0; i < REPS; i++)
			(void)rgb565_qoi_decompress(s_comp, n, s_out, IMG_PIXELS);
		us = time_us_32() - t0;
		if (ok)
			report("QOI", us, n);
		else
			printf("[bench] QOI round-trip FAILED, no timing reported\n");
	}

	/* ---- RLE ---- */
	n = rgb565_rle_compress(s_src, IMG_PIXELS, s_comp, sizeof(s_comp));
	if (!n) {
		printf("[bench] RLE compress failed\n");
	} else {
		memset(s_out, 0, sizeof(s_out));
		ok = rgb565_rle_decompress(s_comp, n, s_out, IMG_PIXELS) == IMG_PIXELS &&
		     memcmp(s_src, s_out, sizeof(s_src)) == 0;
		t0 = time_us_32();
		for (i = 0; i < REPS; i++)
			(void)rgb565_rle_decompress(s_comp, n, s_out, IMG_PIXELS);
		us = time_us_32() - t0;
		if (ok)
			report("RLE", us, n);
		else
			printf("[bench] RLE round-trip FAILED, no timing reported\n");
	}

	/* ---- LZ4 (a stored-block style stream: the device's LZ4 path takes a
	 * raw block, so this is what the host would send it) ---- */
	n = (size_t)LZ4_compress_default((const char *)s_src, (char *)s_comp,
	                                 (int)sizeof(s_src), (int)sizeof(s_comp));
	if (!n) {
		printf("[bench] LZ4 compress failed\n");
	} else {
		memset(s_out, 0, sizeof(s_out));
		ok = LZ4_decompress_safe((const char *)s_comp, (char *)s_out, (int)n,
		                         (int)sizeof(s_out)) == (int)sizeof(s_src) &&
		     memcmp(s_src, s_out, sizeof(s_src)) == 0;
		t0 = time_us_32();
		for (i = 0; i < REPS; i++)
			(void)LZ4_decompress_safe((const char *)s_comp, (char *)s_out,
			                          (int)n, (int)sizeof(s_out));
		us = time_us_32() - t0;
		if (ok)
			report("LZ4", us, n);
		else
			printf("[bench] LZ4 round-trip FAILED, no timing reported\n");
	}

	printf("[bench] done\n");
	for (;;)
		tight_loop_contents();
}
