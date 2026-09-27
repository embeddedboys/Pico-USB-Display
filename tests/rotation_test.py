#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
Walk the panel through its orientations and show what each one looks like.

This is the standalone rotation test: it needs no kernel driver (pyusb) and it
exercises the whole chain rather than just the register write --

  1. ask the device to rotate (PUD_CMD_SET_PARAM, the same request a driver
     sends at probe),
  2. read the capability report back and take the frame size from it, which is
     exactly what a driver does before it builds its mode,
  3. draw an asymmetric pattern at that size -- a green arrow pointing at what
     the *logical* top edge is, plus the rotation number and the frame size --
     and send it over EP1 like any other frame.

So if the arrow points up and the text reads upright in every orientation, the
rotation, the geometry bookkeeping and a host's use of it are all correct.  The
picture is redrawn for each orientation because the frame size changes with it.

Usage:
    ./tests/rotation_test.py                  # 0, 1, 2, 3 and back, 2 s each
    ./tests/rotation_test.py --rotation 0     # only this one, left in place
    ./tests/rotation_test.py --cycle 2 --delay 3
    ./tests/rotation_test.py --image assets/xfce.jpg   # a real picture instead

The device must be built with the matching DECODER_TYPE (the script follows what
the capability report says) and must not be held by the kernel driver:
`scripts/pud-load.sh unload` on the machine that has it loaded.
'''

import argparse
import os
import sys
import time

# pud_usb lives with the tools, one level up from the tests
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, "tools"))
import pud_usb


def pattern(width, height, rotation):
    """An asymmetric frame: only its orientation tells you which way is up."""
    from PIL import Image, ImageDraw, ImageFont

    img = Image.new("RGB", (width, height), (16, 16, 24))
    d = ImageDraw.Draw(img)

    d.rectangle([0, 0, width - 1, height - 1], outline=(255, 255, 255), width=3)
    # arrow at the logical top edge, so a wrong orientation is obvious
    cx = width // 2
    d.polygon([(cx, 6), (cx - width // 10, height // 5),
               (cx + width // 10, height // 5)], fill=(0, 255, 0))
    d.text((cx - width // 12, height // 5 + 4), "TOP", fill=(255, 0, 0),
           font=_font(max(12, width // 16)))

    label = "rotation %d" % rotation
    size = "%dx%d" % (width, height)
    d.text((10, height - 34), label, fill=(255, 255, 0),
           font=_font(max(12, width // 20)))
    d.text((10, height - 34 - max(14, width // 18)), size, fill=(0, 200, 255),
           font=_font(max(12, width // 20)))
    return img


def _font(size):
    from PIL import ImageFont

    for path in ("/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                 "/usr/share/fonts/TTF/DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


def codec_name(decoder_type):
    for name, value in pud_usb.DECODER_TYPES.items():
        if value == decoder_type:
            return name
    return "qoi"


def show(disp, width, height, rotation, codec, image):
    if image:
        raw = pud_usb.load_image(image, width, height)
        rgb565 = pud_usb.rgb888_to_rgb565(raw, width, height)
    else:
        rgb565 = pud_usb.rgb888_to_rgb565(pattern(width, height, rotation)
                                          .tobytes(), width, height)

    disp.width, disp.height = width, height
    bands, payload, secs = disp.send_rgb565(rgb565, width, height, codec=codec)
    print("           frame %dx%d, %d band(s), %d bytes, %.3f s"
          % (width, height, bands, payload, secs))


def main():
    ap = argparse.ArgumentParser(description="standalone rotation test")
    ap.add_argument("--rotation", type=int, default=None,
                    help="test only this rotation (0..3) and leave it set")
    ap.add_argument("--cycle", type=int, default=1,
                    help="how many times to walk 0,1,2,3 (default 1)")
    ap.add_argument("--delay", type=float, default=2.0,
                    help="seconds to leave each orientation on the panel")
    ap.add_argument("--image", default=None,
                    help="send this picture instead of the arrow pattern")
    args = ap.parse_args()

    try:
        with pud_usb.open_device() as disp:
            caps = disp.query_caps()
            start = caps["rotation"]
            codec = codec_name(disp.decoder_type)
            print("device reports rotation %d, %dx%d, decoder %s"
                  % (start, caps["xres"], caps["yres"], codec))

            rotations = [args.rotation] if args.rotation is not None \
                else [0, 1, 2, 3]

            for _ in range(args.cycle):
                for rot in rotations:
                    disp.set_params(mask=pud_usb.PARAM_ROTATION, rotation=rot)
                    state = disp.get_params()
                    if state["rejected"] & pud_usb.PARAM_ROTATION:
                        sys.exit("device rejected rotation %d" % rot)
                    if state["rotation"] != rot:
                        sys.exit("read-back mismatch: asked %d, got %d"
                                 % (rot, state["rotation"]))

                    caps = disp.query_caps()
                    print("rotation %d -> geometry %dx%d"
                          % (rot, caps["xres"], caps["yres"]))
                    show(disp, caps["xres"], caps["yres"], rot, codec,
                         args.image)

                    if args.delay:
                        time.sleep(args.delay)

            if args.rotation is None and start != rotations[-1]:
                disp.set_params(mask=pud_usb.PARAM_ROTATION, rotation=start)
                print("restored rotation %d" % start)
    except pud_usb.PudError as exc:
        sys.exit(str(exc))


if __name__ == "__main__":
    main()
