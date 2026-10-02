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
#include <string.h>

// #include "udd.h"
#include "tft.h"
#include "backlight.h"
#include "decoder.h"
#include "decoder_internal.h"
#include "lz4.h"
#include "usb.h"
#include "pud.h" /* PUD_EP1_HEADER_SIZE, the EP1 framing */
#if DECODER_TYPE == DECODER_USE_QOIZ
/* QOI+deflate has no logo branch of its own: the logo is plain QOI, drawn with
 * qoi_drawimg() (see decoder_draw_bootlogo), so take the QOI array while
 * bootlogo.h is being included.  Each type restates its own restore rather than
 * stashing the old value in another macro: a macro body is expanded where it is
 * used, so a stashed copy would come back as the token DECODER_TYPE again. */
#undef DECODER_TYPE
#define DECODER_TYPE DECODER_USE_QOI
#include "bootlogo.h"
#undef DECODER_TYPE
#define DECODER_TYPE DECODER_USE_QOIZ
#elif DECODER_TYPE == DECODER_USE_QOID
/* same trick: the dictionary variant decodes QOI underneath too */
#undef DECODER_TYPE
#define DECODER_TYPE DECODER_USE_QOI
#include "bootlogo.h"
#undef DECODER_TYPE
#define DECODER_TYPE DECODER_USE_QOID
#else
#include "bootlogo.h"
#endif

#include "pico/time.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/*
 * Decode one band with whichever codec this firmware was built for.
 *
 * A function rather than the #define chain that used to be in decoder.h: that
 * chain made every caller read a seven-branch #if to find out what actually
 * ran, and it needed a second macro (decoder_drawimg_slot) because
 * DECODER_TYPE 6 alone wants to know which slot the band came out of.  A
 * compile-time constant folds this switch away, so the indirection costs
 * nothing at run time -- which is what the macros were buying.
 *
 * `slot` and `serial` describe where the band came from (see
 * decoder_submit_frame); only DECODER_TYPE 6 reads them.
 */
void decoder_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *data, u32 size,
                     int slot, u32 serial)
{
#if DECODER_TYPE == DECODER_USE_TJPGD
	tjpgd_drawimg(xs, ys, xe, ye, data, size);
#elif DECODER_TYPE == DECODER_USE_JPEGDEC
	jpegdec_drawimg(xs, ys, xe, ye, data, size);
#elif DECODER_TYPE == DECODER_USE_LZ4
	lz4_drawimg(xs, ys, xe, ye, data, size);
#elif DECODER_TYPE == DECODER_USE_QOI
	qoi_drawimg(xs, ys, xe, ye, data, size);
#elif DECODER_TYPE == DECODER_USE_RLE
	rle_drawimg(xs, ys, xe, ye, data, size);
#elif DECODER_TYPE == DECODER_USE_QOIZ
	qoiz_drawimg(xs, ys, xe, ye, data, size);
#elif DECODER_TYPE == DECODER_USE_QOID
	qoid_drawimg(xs, ys, xe, ye, data, size, slot, serial);
#else
#error "Invalid decoder type selected"
#endif
}




struct decoder_frame {
	u16 xs, ys, xe, ye;
	u32 size;
	/* Position of this band in the accepted sequence.  DECODER_TYPE 6 needs
	 * it: the payload says which band its dictionary was built from, and the
	 * slot compares that against what it actually holds. */
	u32 serial;
	u8 busy;
	u8 data[DECODER_FRAME_MAX];
};

static struct decoder_frame s_frames[DECODER_FRAME_SLOTS];
/*
 * Slots are handed out lowest-free-first and drained in index order (see
 * decoder_submit_frame() and decoder_task()).  "The band submitted
 * DECODER_FRAME_SLOTS ago" therefore only holds while the pipeline is
 * saturated; a round-robin fill/drain cursor pair was tried and stalled the
 * pipeline under a full-screen load (303 submitted, 300 drawn, decoder_task
 * blocked), so DECODER_TYPE 6 finds its dictionary by the serial the payload
 * names instead of by slot arithmetic -- notes/decoders.md and notes/todo.md
 * item 13.
 */


static SemaphoreHandle_t s_decoder_sem;

/* Diagnostics, readable from a debugger: frames seen / dropped / drawn.
 * A non-zero drop count means the host is outrunning the decoder and some
 * partial updates never reach the panel (visible as stale regions).
 * `oversize` counts transfers that did not fit a frame slot: the control
 * stage (see usbd_vendor_ep1_size_ok) is supposed to refuse those first, so a
 * non-zero value means device and host disagree about the frame size. */
volatile u32 g_decoder_stat_submitted;
volatile u32 g_decoder_stat_dropped;
volatile u32 g_decoder_stat_drawn;
volatile u32 g_decoder_stat_oversize;

/* True when at least one frame slot is idle, i.e. the USB stack may arm
 * EP1 for the next frame.  Used for EP1 flow control (see usb.h). */
bool decoder_slot_free(void)
{
	int i;

	for (i = 0; i < DECODER_FRAME_SLOTS; i++) {
		if (!s_frames[i].busy)
			return true;
	}

	return false;
}

void decoder_submit_frame(u16 xs, u16 ys, u16 xe, u16 ye, const u8 *data,
                          u32 size)
{
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	int i;

	/* Drop rather than truncate.  The old code clamped the size and decoded
	 * a truncated stream, which corrupted the image with no error visible
	 * anywhere.  The control stage refuses oversized transfers first, so
	 * this is a last line of defence. */
	if (size > DECODER_FRAME_MAX) {
		g_decoder_stat_oversize++;
		return;
	}

	g_decoder_stat_submitted++;

	/*
	 * Lowest free slot, which is *not* the round-robin rotation DECODER_TYPE 6
	 * would like: its dictionary lives in the slot, so the host would like to
	 * know which band is resident where.  Slot k only holds the band submitted
	 * k % DECODER_FRAME_SLOTS ago while the pipeline is saturated; a host slow
	 * enough to keep one band in flight puts every band in slot 0 (measured:
	 * 8 of 15 deltas refused).  A fill/drain cursor pair was tried and shown
	 * to stall the pipeline under a full-screen load (303 submitted, 300
	 * drawn, decoder_task blocked), so the device now finds the dictionary by
	 * the serial the payload names -- see notes/decoders.md and notes/todo.md
	 * item 13.
	 */
	for (i = 0; i < DECODER_FRAME_SLOTS; i++) {
		if (!s_frames[i].busy) {
			s_frames[i].xs = xs;
			s_frames[i].ys = ys;
			s_frames[i].xe = xe;
			s_frames[i].ye = ye;
			s_frames[i].size = size;
			s_frames[i].serial = g_decoder_stat_submitted - 1;
			memcpy(s_frames[i].data, data, size);
			s_frames[i].busy = 1;
			xSemaphoreGiveFromISR(s_decoder_sem,
			                      &xHigherPriorityTaskWoken);
			break;
		}
	}

	if (i == DECODER_FRAME_SLOTS)
		g_decoder_stat_dropped++;

	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/*
 * Draw the boot logo.
 *
 * QOI, RLE and both JPEG decoders store one stream for the whole panel, so the
 * logo is just the first frame.  LZ4 cannot: a single block would have to be
 * decoded in one piece (see lz4_drawimg()), so its logo is stored the way the
 * device consumes it -- the same [count][offsets][data...] container the
 * conversion tool writes for a frame sequence, one block per band, and the
 * bands are uniform so the row height follows from the count.  Regenerating it
 * is `pudcodec video2s --raw <raw565> -w 480 -h <rows> --codec lz4 -t bin`;
 * see notes/scripts.md.
 */
static void decoder_draw_bootlogo(void)
{
	/* The logo is baked for the orientation this firmware was built in: it is
	 * one image, bands and all, with no second copy for the other way round.
	 * A host that rotated the panel at runtime (PUD_CMD_SET_PARAM) gets no
	 * logo rather than a rotated one -- the first frame it sends is what it
	 * asked for anyway. */
	if (g_pud_data.disp.rotation != TFT_ROTATION)
		return;

#if DECODER_TYPE == DECODER_USE_LZ4
	const u8 *p = (const u8 *)bootlogo;
	u32 count = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
	            ((u32)p[3] << 24);
	u32 rows;
	u32 i;

	if (count == 0 || count > TFT_VER_RES || TFT_VER_RES % count != 0)
		return; /* not a container: nothing sane to draw */
	rows = TFT_VER_RES / count;

	for (i = 0; i < count; i++) {
		const u8 *e = p + 4u + 4u * (i + 1u);
		u32 start = (u32)p[4u + 4u * i] |
		            ((u32)p[4u + 4u * i + 1u] << 8) |
		            ((u32)p[4u + 4u * i + 2u] << 16) |
		            ((u32)p[4u + 4u * i + 3u] << 24);
		u32 end = (u32)e[0] | ((u32)e[1] << 8) | ((u32)e[2] << 16) |
		          ((u32)e[3] << 24);
		u32 y = i * rows;

		if (end <= start || end > sizeof(bootlogo))
			break;
		lz4_drawimg(0, (u16)y, TFT_HOR_RES - 1,
		            (u16)(i + 1u == count ? TFT_VER_RES - 1 :
		                                    y + rows - 1),
		            (u8 *)p + start, end - start);
	}
#elif DECODER_TYPE == DECODER_USE_QOIZ || DECODER_TYPE == DECODER_USE_QOID
	/* stored as plain QOI, see the bootlogo.h include */
	qoi_drawimg(0, 0, TFT_HOR_RES - 1, TFT_VER_RES - 1, (uint8_t *)bootlogo,
	            sizeof(bootlogo));
#else
	/* No slot: this runs before any host band, so it is a keyframe. */
	decoder_drawimg(0, 0, TFT_HOR_RES - 1, TFT_VER_RES - 1,
	                (uint8_t *)bootlogo, sizeof(bootlogo), 0, 0);
#endif
}

static void decoder_task(void *param)
{
	int i;

	(void)param;

	/* Whatever the host set over USB that has to reach the panel (a rotation:
	 * the MADCTL write is not something to do in the USB interrupt) is applied
	 * here, before anything is drawn -- otherwise the logo and the first frame
	 * would be drawn in the orientation the panel still had. */
	pud_params_flush_display();

	/* First paint: the boot logo, before any host frame.  The panel has
	 * exactly one writer (this task), so no lock is needed, and doing it
	 * here instead of in a task of its own both drops a task and makes
	 * "the logo is the first thing drawn" a hard guarantee.  The USB side
	 * is independent: enumeration runs on core 0 while this draws. */
	decoder_draw_bootlogo();

	busy_wait_ms(10);
	backlight_set_level(100);
	printf("backlight set to 100%%\n");

	for (;;) {
		int slot = -1;

		/* Take stock before sleeping.  The submit side signals a binary
		 * semaphore, so two frames arriving close together can collapse
		 * into a single give; scanning the slots first draws the second
		 * frame now instead of after the 200 ms wait below. */
		for (i = 0; i < DECODER_FRAME_SLOTS; i++) {
			if (s_frames[i].busy) {
				slot = i;
				break;
			}
		}

		if (slot < 0) {
			/* Nothing queued.  The timed wait doubles as the EP1
			 * caretaker: it drops a transfer whose host went away
			 * and re-arms a read that was lost. */
			if (xSemaphoreTake(s_decoder_sem,
			                   pdMS_TO_TICKS(EP1_POLL_PERIOD_MS)) != pdTRUE)
				usbd_vendor_ep1_poll();
			/* a rotation asked for while idle still has to land */
			pud_params_flush_display();
			continue;
		}

		/* Before this frame, not after: a rotation the host asked for
		 * arrives together with the mode change that follows it, and the
		 * frame it sends next is already in the new orientation. */
		pud_params_flush_display();

		decoder_drawimg(s_frames[slot].xs, s_frames[slot].ys,
		                s_frames[slot].xe, s_frames[slot].ye,
		                s_frames[slot].data, s_frames[slot].size, slot,
		                s_frames[slot].serial);
		s_frames[slot].busy = 0;
		g_decoder_stat_drawn++;
		/* A slot is free again: re-arm EP1 if the host's request was
		 * deferred. */
		usbd_vendor_ep1_tick();
	}
}

/* The two QOI+deflate entries decode the same stream, so they share one
 * DECODER_TYPE value and differ only in what the firmware was built with. */
#if PUD_INFLATE == 2
#define DECODER_NAME_QOIZ "QOI+deflate (libdeflate)"
#else
#define DECODER_NAME_QOIZ "QOI+deflate (tinfl)"
#endif

/* Indexed by DECODER_TYPE.  The numbering must not shift: the device reports
 * this value to the host through PUD_CMD_GET_CAPS. */
static const char *const decoder_names[] = {
	"tjpgd", "JPEGDEC", "LZ4", "QOI", "RLE", DECODER_NAME_QOIZ,
	"QOI+deflate+dict",
};
_Static_assert(DECODER_TYPE < sizeof(decoder_names) / sizeof(decoder_names[0]),
               "decoder_names[] has to cover every DECODER_TYPE");

/*
 * 1024 words = 4 KB.  Measured peak of the whole decode path (0xa5 fill scan,
 * see notes/debugging.md): JPEGDEC 632 B for a full 480x320 image, QOI 496 B.
 * The old 4096 words was sized on the assumption that JPEGDEC eats stack, which
 * the measurement does not support: JPEGDEC keeps its context in .bss
 * (&g_jpegdec) and its MCU callback only uses scalars.  4 KB keeps ~6x margin
 * over the worst case, which matters on RP2040 (264 KB SRAM, and no PSPLIM
 * stack guard on Cortex-M0+).
 */
#define DECODER_TASK_STACK_WORDS 1024

void decoder_init(void)
{
	BaseType_t created;

	s_decoder_sem = xSemaphoreCreateBinary();

	created = xTaskCreate(decoder_task, "decoder_task",
	                      DECODER_TASK_STACK_WORDS, NULL,
	                      tskIDLE_PRIORITY + 1, NULL);
	if (created != pdPASS) {
		/*
		 * Without this the failure is silent from both ends: bands are
		 * accepted until the slots fill, and then nothing drains one, so
		 * EP1 flow control holds the host forever and the panel simply
		 * stops updating.  Say so and stop, the way check_task_created()
		 * in main.c does.  Note this runs from pud_init(), before
		 * watchdog_enable(), so the stop is a bench-visible one: the
		 * self-healing watchdog is not armed yet.
		 */
		printf("FATAL: xTaskCreate(decoder_task) failed, no memory\n");
		__breakpoint();
	}

	printf("Decoder type: %s\n", decoder_names[DECODER_TYPE]);
}
