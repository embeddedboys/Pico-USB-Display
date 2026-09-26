#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
Display a still image on the Pico USB Display.

Images are decoded with Pillow (or opencv-python if Pillow is missing),
converted to RGB565, compressed with the codec the device is built for
(--codec, default qoi) and sent over EP1 -- the same pipeline the kernel
driver uses; banding is `pud_usb.send_rgb565()`'s job.

The device has to be built with the matching DECODER_TYPE (see
notes/scripts.md); this checks the capability report and refuses to send a
codec the device would drop without an error.

Usage:
    ./scripts/img_viewer.py [options] <image>

Options:
    --xres W        panel width  (default 480)
    --yres H        panel height (default 320)
    --width W       image window width  (default: the panel width)
    --height H      image window height (default: the panel height)
    --x X, --y Y    place the window at (X,Y) instead of the top-left corner
    --stretch       fill the window instead of preserving the aspect ratio
    --repeat N      send the frame N times (default 1)
    --codec NAME    what the device is built for: qoi (default), lz4, rle
    --timer         print per-send timing

Examples:
    ./scripts/img_viewer.py assets/xfce.jpg
    ./scripts/img_viewer.py --width 160 --height 120 --x 100 --y 60 -r 50 \\
        assets/bootlogo.png
    ./scripts/img_viewer.py --codec lz4 assets/xfce.jpg     # DECODER_TYPE=2
'''

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pud_usb


def main():
    ap = argparse.ArgumentParser(description="show an image on the panel")
    ap.add_argument("image")
    ap.add_argument("--xres", type=int, default=480, help="panel width")
    ap.add_argument("--yres", type=int, default=320, help="panel height")
    ap.add_argument("--width", type=int, default=None, help="window width")
    ap.add_argument("--height", type=int, default=None, help="window height")
    ap.add_argument("--x", type=int, default=0, help="window origin x")
    ap.add_argument("--y", type=int, default=0, help="window origin y")
    ap.add_argument("--stretch", action="store_true",
                    help="stretch to the window instead of letterboxing")
    ap.add_argument("--repeat", "-r", type=int, default=1)
    ap.add_argument("--codec", default="qoi", choices=sorted(pud_usb.ENCODERS),
                    help="what the device is built for (DECODER_TYPE)")
    ap.add_argument("--timer", action="store_true")
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
            want = pud_usb.DECODER_TYPES[args.codec]
            if disp.decoder_type is not None and disp.decoder_type != want:
                sys.exit("the device reports decoder_type=%s, not %d (%s); "
                         "rebuild it with DECODER_TYPE=%d or pass a matching "
                         "--codec"
                         % (disp.decoder_type, want, args.codec, want))
            total = 0
            for _ in range(args.repeat):
                bands, nbytes, secs = disp.send_rgb565(
                    rgb565, win_w, win_h, args.x, args.y,
                    codec=args.codec)
                total += nbytes
                if args.timer or args.repeat == 1:
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
