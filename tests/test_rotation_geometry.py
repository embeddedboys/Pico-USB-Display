#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
Walking the panel through its orientations: the reported geometry must follow
the rotation, and a frame drawn at that geometry must reach the panel in one
piece at every orientation.

This is the standalone rotation check and needs no kernel driver.  It exercises
the whole chain rather than just a register write:

  1. ask the device to rotate (`PUD_CMD_SET_PARAM`, the request a driver sends
     at probe), and check the read-back;
  2. read the capability report back and take the frame size from it -- exactly
     what a driver does before building its mode;
  3. draw an asymmetric pattern at that size (a green arrow at the logical top
     edge, plus the rotation number and the frame size) and send it over EP1.

If the arrow points up and the text reads upright in every orientation, the
rotation, the geometry bookkeeping and a host's use of it are all correct.  The
verdict itself is derived: geometry parity, message acceptance, and the
device's own counters -- the "arrow looks right" part is a human observation
and is reported as such.

    python3 tests/test_rotation_geometry.py
    python3 tests/test_rotation_geometry.py --rotation 0     # one, left set
    python3 tests/test_rotation_geometry.py --cycle 2 --delay 3
    python3 tests/test_rotation_geometry.py --image assets/xfce.jpg

The firmware must be built with the matching DECODER_TYPE (the script follows
what the capability report says) and must not be held by the kernel driver.
'''

import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "common"))
sys.path.insert(0, os.path.join(_HERE, "tools"))

from harness import Oracle, Report, Snapshot, Test, run_test  # noqa: E402
import pud_usb  # noqa: E402

# ORACLE: SPEC
# SOURCE: notes/usb-protocol.md -- the rotation->geometry table and the
#         "swap width/height when the parity differs from the compile-time
#         orientation" rule; the reported geometry is what the host builds its
#         DRM mode from, so it is a contract, not a preference
# EXPECTED: for each requested rotation the device reads back that rotation,
#           reports rotation in caps, and reports the geometry the table gives
#           (320x480 for even rotations, 480x320 for odd); every frame at that
#           geometry is accepted
ORACLE = Oracle(
    "SPEC",
    "notes/usb-protocol.md (rotation->geometry table: 0/2 -> 320x480, "
    "1/3 -> 480x320 on this ILI9488 panel) and the capability-report contract",
    "rotation reads back; caps rotation follows; geometry follows the parity "
    "rule; a frame at each geometry is accepted by the firmware")

GEOMETRY_BY_PARITY = {0: (320, 480), 1: (480, 320)}


def extra_args(ap):
    ap.add_argument("--rotation", type=int, default=None, choices=range(4),
                    help="test only this rotation and leave it set")
    ap.add_argument("--cycle", type=int, default=1,
                    help="how many times to walk 0,1,2,3 (default 1)")
    ap.add_argument("--delay", type=float, default=1.0,
                    help="seconds to leave each orientation on the panel")
    ap.add_argument("--image", default=None,
                    help="send this picture instead of the arrow pattern")


def _font(size):
    from PIL import ImageFont

    for path in ("/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                 "/usr/share/fonts/TTF/DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


def pattern(width, height, rotation):
    """An asymmetric frame: only its orientation tells you which way is up."""
    from PIL import Image, ImageDraw

    img = Image.new("RGB", (width, height), (16, 16, 24))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, width - 1, height - 1], outline=(255, 255, 255), width=3)
    cx = width // 2
    d.polygon([(cx, 6), (cx - width // 10, height // 5),
               (cx + width // 10, height // 5)], fill=(0, 255, 0))
    d.text((cx - width // 12, height // 5 + 4), "TOP", fill=(255, 0, 0),
           font=_font(max(12, width // 16)))
    d.text((10, height - 34), "rotation %d" % rotation, fill=(255, 255, 0),
           font=_font(max(12, width // 20)))
    d.text((10, height - 34 - max(14, width // 18)), "%dx%d" % (width, height),
           fill=(0, 200, 255), font=_font(max(12, width // 20)))
    return img


def codec_name(decoder_type):
    for name, value in pud_usb.DECODER_TYPES.items():
        if value == decoder_type:
            return name
    return "qoi"


def check(dev, args):
    snaps, failures = [], []
    caps = dev.query_caps()
    start = caps["rotation"]
    codec = codec_name(dev.decoder_type)
    snaps.append(Snapshot("start", "rotation %d, %dx%d, decoder %s"
                          % (start, caps["xres"], caps["yres"], codec)))

    rotations = [args.rotation] if args.rotation is not None else [0, 1, 2, 3]

    try:
        for _ in range(args.cycle):
            for rot in rotations:
                dev.set_params(mask=pud_usb.PARAM_ROTATION, rotation=rot)
                state = dev.get_params()
                if state["rejected"] & pud_usb.PARAM_ROTATION:
                    failures.append("device rejected rotation %d" % rot)
                    continue
                if state["rotation"] != rot:
                    failures.append("rotation read-back %d != %d"
                                    % (state["rotation"], rot))

                caps = dev.query_caps()
                want = GEOMETRY_BY_PARITY[rot % 2]
                got = (caps["xres"], caps["yres"])
                snaps.append(Snapshot("rotation_%d" % rot,
                                      "geometry %dx%d" % got))
                if got != want:
                    failures.append("rotation %d should report %dx%d, device "
                                    "reports %dx%d"
                                    % (rot, want[0], want[1], got[0], got[1]))

                width, height = got
                if args.image:
                    raw = pud_usb.load_image(args.image, width, height)
                    rgb565 = pud_usb.rgb888_to_rgb565(raw, width, height)
                else:
                    rgb565 = pud_usb.rgb888_to_rgb565(
                        pattern(width, height, rot).tobytes(), width, height)
                dev.width, dev.height = width, height
                bands, payload, secs = dev.send_rgb565(rgb565, width, height,
                                                       codec=codec)
                snaps.append(Snapshot("rotation_%d_frame" % rot,
                                      "%d band(s), %d bytes, %.3f s"
                                      % (bands, payload, secs)))
                if args.delay:
                    time.sleep(args.delay)
    finally:
        if args.rotation is None and start != rotations[-1]:
            try:
                dev.set_params(mask=pud_usb.PARAM_ROTATION, rotation=start)
            except Exception:
                pass

    if args.rotation is None and start != rotations[-1]:
        snaps.append(Snapshot("restored_rotation", start))

    return Report.of(
        snaps, ok=not failures,
        detail=("; ".join(failures) if failures
                else "geometry followed every rotation; verify by eye that the "
                     "arrow points along the logical top edge in each frame"))


TEST = Test(
    "test_rotation_geometry", ORACLE, check,
    description="rotation -> caps geometry -> frame acceptance (needs device)")
TEST.extra_args = extra_args

if __name__ == "__main__":
    run_test(TEST)
