#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
Display an image with the LZ4 decoder.

Requires firmware built with DECODER_TYPE=2 (LZ4), which is NOT the default
-- QOI (DECODER_TYPE=3) is the default and has its own viewer.

Two limits come from the firmware side and are checked here rather than
silently truncating the frame:

  * lz4_drawimg() decompresses the whole stream into a full-frame workspace
    and then flushes the window, so only whole-panel frames are supported;
  * the compressed stream has to fit one transfer (65535 bytes) and thus the
    firmware's 64 KB frame slot.

A 480x320 RGB565 frame is 307200 raw bytes, so LZ4 has to reach better than
5:1 for this to work -- fine for flat graphics, usually not for photos.

Usage:
    ./scripts/lz4_img_viewer.py [options] <image>

Options:
    --xres W, --yres H   panel size (default 480x320)
    --repeat N           send the frame N times (default 1)
    --stretch            stretch to the panel instead of letterboxing

Requires the python lz4 package:
    pip install lz4
'''

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pud_usb


def main():
    ap = argparse.ArgumentParser(description="show an image via the LZ4 decoder")
    ap.add_argument("image")
    ap.add_argument("--xres", type=int, default=480)
    ap.add_argument("--yres", type=int, default=320)
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--stretch", action="store_true")
    args = ap.parse_args()

    try:
        import lz4.block
    except ImportError:
        sys.exit("the lz4 package is required: pip install lz4")

    try:
        raw = pud_usb.load_image(args.image, args.xres, args.yres,
                                 fit=not args.stretch)
        rgb565 = pud_usb.rgb888_to_rgb565(raw, args.xres, args.yres)

        # store_size=False gives the bare LZ4 block the firmware expects; the
        # decompressed length is implied by the window instead.
        payload = lz4.block.compress(rgb565, store_size=False)

        print("raw %d bytes, lz4 %d bytes (%.2f:1)"
              % (len(rgb565), len(payload), len(rgb565) / len(payload)))

        if len(payload) > pud_usb.USB_TRANS_MAX_SIZE:
            sys.exit("compressed frame is %d bytes, over the %d byte transfer "
                     "limit -- the firmware cannot take a frame this size. "
                     "Use a smaller panel size or simpler content."
                     % (len(payload), pud_usb.USB_TRANS_MAX_SIZE))

        with pud_usb.open_device() as disp:
            for i in range(args.repeat):
                nbytes, secs = disp.send_raw(payload, 0, 0, args.xres - 1,
                                             args.yres - 1)
                if args.repeat == 1:
                    print("sent %d bytes in %.1f ms" % (nbytes, secs * 1e3))
            if args.repeat > 1:
                print("%d frames sent" % args.repeat)
    except pud_usb.PudError as exc:
        sys.exit(str(exc))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
