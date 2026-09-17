#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# Copyright (c) 2020 2024 Daniel Gorbea
#
# Copyright (c) 2020 Raspberry Pi (Trading) Ltd. author of https://github.com/raspberrypi/pico-examples/tree/master/usb
#
# Copyright (c) 2025 Liyulingyue
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
Mirror an X11 screen onto the Pico USB Display.

The screen is grabbed with ffmpeg's x11grab, so no python X binding is
needed (the previous version imported Xlib and opened a fresh Display every
frame, which leaked a connection per frame). Only the bounding box of what
actually changed is sent, so a mostly static desktop costs almost nothing
and full-screen video is limited by the link rather than by the sender.

Requires firmware built with DECODER_TYPE=3 (QOI), which is the default.

Usage:
    ./scripts/xorg_desktop_share.py [options]

Options:
    --xres W, --yres H   panel size (default 480x320)
    --fps N              capture rate (default 15)
    --display D          X display (default $DISPLAY)
    --stretch            stretch to the panel instead of letterboxing
    --no-diff            always send the whole frame
    --frames N           stop after N frames
    --stats              print a line every second

x11grab only works on X11. On a Wayland session run the compositor's own
screencast tool, or use a nested X server (Xephyr/Xwayland) to share.

Examples:
    ./scripts/xorg_desktop_share.py --fps 10
    ./scripts/xorg_desktop_share.py --stretch --stats
'''

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pud_usb


def changed_bbox(old, new, width, height):
    """Smallest (x1, y1, x2, y2) covering every changed pixel, or None.

    Comparison works on the packed RGB565 bytes; identical rows are rejected
    with a single C-level bytes compare.
    """
    stride = width * 2
    first = -1
    last = -1
    for y in range(height):
        if old[y * stride:(y + 1) * stride] != new[y * stride:(y + 1) * stride]:
            if first < 0:
                first = y
            last = y
    if first < 0:
        return None

    try:
        import numpy as np

        a = np.frombuffer(old, np.uint8).reshape(height, width * 2)
        b = np.frombuffer(new, np.uint8).reshape(height, width * 2)
        cols = np.flatnonzero((a[first:last + 1] != b[first:last + 1]).any(axis=0))
        return int(cols[0]) // 2, first, int(cols[-1]) // 2, last
    except ImportError:
        pass

    x1, x2 = width, -1
    for y in range(first, last + 1):
        a = old[y * stride:(y + 1) * stride]
        b = new[y * stride:(y + 1) * stride]
        if a == b:
            continue
        va = memoryview(a).cast("H")
        vb = memoryview(b).cast("H")
        for x in range(width):
            if va[x] != vb[x]:
                if x < x1:
                    x1 = x
                if x > x2:
                    x2 = x
    if x2 < 0:
        return None
    return x1, first, x2, last


def grab_cmd(display, width, height, fps, stretch):
    filters = ["fps=%s" % fps]
    if stretch:
        filters.append("scale=%d:%d" % (width, height))
    else:
        filters.append("scale=%d:%d:force_original_aspect_ratio=decrease"
                       % (width, height))
        filters.append("pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=black"
                       % (width, height))
    return ["ffmpeg", "-v", "error", "-f", "x11grab",
            "-i", display, "-vf", ",".join(filters),
            "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]


def main():
    ap = argparse.ArgumentParser(description="mirror an X11 screen to the panel")
    ap.add_argument("--xres", type=int, default=480)
    ap.add_argument("--yres", type=int, default=320)
    ap.add_argument("--fps", type=float, default=15.0)
    ap.add_argument("--display", default=os.environ.get("DISPLAY", ":0.0"))
    ap.add_argument("--stretch", action="store_true")
    ap.add_argument("--no-diff", action="store_true")
    ap.add_argument("--frames", type=int, default=None)
    ap.add_argument("--stats", action="store_true")
    args = ap.parse_args()

    if not os.environ.get("DISPLAY"):
        print("note: DISPLAY is not set (session type: %s) -- x11grab needs an "
              "X server; pass --display if it lives elsewhere"
              % os.environ.get("XDG_SESSION_TYPE", "unknown"),
              file=sys.stderr)

    cmd = grab_cmd(args.display, args.xres, args.yres, args.fps, args.stretch)

    try:
        with pud_usb.open_device() as disp:
            disp.width, disp.height = args.xres, args.yres
            prev = None
            sent = 0
            t0 = time.perf_counter()
            t_report = t0
            bytes_sent = 0

            for raw in pud_usb.ffmpeg_frames(cmd, args.xres, args.yres):
                cur = pud_usb.rgb888_to_rgb565(raw, args.xres, args.yres)

                if args.no_diff or prev is None:
                    box = (0, 0, args.xres - 1, args.yres - 1)
                else:
                    box = changed_bbox(prev, cur, args.xres, args.yres)

                prev = cur
                if box is None:
                    continue

                x1, y1, x2, y2 = box
                w, h = x2 - x1 + 1, y2 - y1 + 1
                patch = pud_usb.crop_rgb565(cur, args.xres, x1, y1, w, h)
                _, nbytes, _ = disp.send_rgb565(patch, w, h, x1, y1)
                bytes_sent += nbytes
                sent += 1

                if args.stats and time.perf_counter() - t_report >= 1.0:
                    el = time.perf_counter() - t0
                    print("%4d frames, %.1f fps, %.2f MB/s, last %dx%d at %d,%d"
                          % (sent, sent / el, bytes_sent / el / 1e6,
                             w, h, x1, y1))
                    t_report = time.perf_counter()

                if args.frames and sent >= args.frames:
                    break

            el = time.perf_counter() - t0
            print("%d frames in %.1f s -> %.1f fps, %.2f MB/s"
                  % (sent, el, sent / el if el else 0,
                     bytes_sent / el / 1e6 if el else 0))
    except pud_usb.PudError as exc:
        sys.exit(str(exc))
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
