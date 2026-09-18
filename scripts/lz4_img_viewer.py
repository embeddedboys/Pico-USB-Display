#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
Display an image with the LZ4 decoder.

Requires firmware built with DECODER_TYPE=2 (LZ4), which is NOT the default
-- QOI (DECODER_TYPE=3) is the default and has its own viewer.  With --codec
this script is a thin wrapper around the banded path in pud_usb, so
`img_viewer.py --codec lz4` does the same thing.

An LZ4 block cannot be decoded in pieces: every match points back at output the
same block produced earlier, so the device decodes exactly one band per
transfer and holds one band of pixels (lz4_drawimg(), see notes/decoders.md).
The banding is therefore not an optimisation here, it is the contract -- a
whole 480x320 frame (307200 raw bytes) would not fit the device's band buffer
however well it compresses.

Usage:
    ./scripts/lz4_img_viewer.py [options] <image>

Options:
    --xres W, --yres H   panel size (default 480x320)
    --width W, --height H  window size (default: the panel)
    --x X, --y Y         window origin (default 0,0)
    --repeat N           send the frame N times (default 1)
    --stretch            stretch to the window instead of letterboxing

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
    ap.add_argument("--width", type=int, default=None, help="window width")
    ap.add_argument("--height", type=int, default=None, help="window height")
    ap.add_argument("--x", type=int, default=0, help="window origin x")
    ap.add_argument("--y", type=int, default=0, help="window origin y")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--stretch", action="store_true")
    args = ap.parse_args()

    if args.repeat < 1:
        sys.exit("--repeat must be >= 1")

    win_w = args.width or args.xres
    win_h = args.height or args.yres

    try:
        raw = pud_usb.load_image(args.image, win_w, win_h,
                                 fit=not args.stretch)
        rgb565 = pud_usb.rgb888_to_rgb565(raw, win_w, win_h)

        with pud_usb.open_device() as disp:
            disp.width, disp.height = args.xres, args.yres
            if disp.decoder_type is not None and disp.decoder_type != 2:
                sys.exit("the device reports decoder_type=%s, not 2 (LZ4); "
                         "rebuild it with DECODER_TYPE=2"
                         % disp.decoder_type)

            total = 0
            for _ in range(args.repeat):
                bands, nbytes, secs = disp.send_rgb565(
                    rgb565, win_w, win_h, args.x, args.y, codec="lz4")
                total += nbytes
                if args.repeat == 1:
                    print("sent %d band(s), %d bytes in %.1f ms (%.2f MB/s)"
                          % (bands, nbytes, secs * 1e3,
                             nbytes / secs / 1e6 if secs else 0))
            if args.repeat > 1:
                print("%d frames, %d bytes total" % (args.repeat, total))
    except pud_usb.PudError as exc:
        sys.exit(str(exc))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
