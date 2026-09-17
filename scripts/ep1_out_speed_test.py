#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
Measure raw EP1 throughput with valid payloads.

Earlier versions of this script sent junk bytes, which the decoder rejects --
so the number it printed described nothing. This version always sends real
QOI streams and sweeps the payload size, which shows the USB ceiling on its
own, independent of how fast the panel can be written.

For the full use-case matrix (full-screen patterns, partial windows, banding
overhead) use fps_bench.py instead.

Usage:
    ./scripts/ep1_out_speed_test.py [--frames N] [--xres W] [--yres H]

Reading the output: throughput saturates at the link rate (about 1.0 MB/s on
full-speed USB). If it drops as the payload grows, the device is not keeping
up with the decode; if it is flat, the link is the limit.
'''

import argparse
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pud_usb

# Window heights as a fraction of the panel, full panel width. Bigger windows
# mean bigger payloads, up to the single-transfer ceiling.
HEIGHT_STEPS = (8, 16, 32, 64, 128, 320)


def main():
    ap = argparse.ArgumentParser(description="EP1 throughput probe")
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--xres", type=int, default=480)
    ap.add_argument("--yres", type=int, default=320)
    args = ap.parse_args()

    try:
        with pud_usb.open_device() as disp:
            disp.width, disp.height = args.xres, args.yres
            print("EP1 throughput, %d frames per size, panel %dx%d"
                  % (args.frames, args.xres, args.yres))
            print("%8s %10s %9s %9s %9s" %
                  ("height", "bytes/frm", "MB/s", "fps", "ms/frame"))

            for h in HEIGHT_STEPS:
                if h > args.yres:
                    continue
                # Deterministic pseudo-random content: incompressible enough
                # that the payload tracks the window size.
                rgba = bytearray(h * args.xres * 3)
                seed = 12345
                for i in range(len(rgba)):
                    seed = (seed * 1103515245 + 12345) & 0xFFFFFFFF
                    rgba[i] = (seed >> 16) & 0xFF
                rgb565 = pud_usb.rgb888_to_rgb565(bytes(rgba), args.xres, h)

                # Encode once, outside the timed loop: this probe is about the
                # link, and host-side QOI encoding would otherwise dominate the
                # number it reports.
                rows = max(1, pud_usb.PUD_MAX_BAND_PIXELS // args.xres)
                rects = [(0, y, args.xres - 1, min(y + rows - 1, h - 1))
                         for y in range(0, h, rows)]
                payloads = []
                for (xs, ys, xe, ye) in rects:
                    patch = pud_usb.crop_rgb565(rgb565, args.xres, xs, ys,
                                                xe - xs + 1, ye - ys + 1)
                    payloads.append((xs, ys, xe, ye, pud_usb.qoi_encode(patch)))
                nbytes = sum(len(p[4]) for p in payloads)

                times = []
                for _ in range(args.frames):
                    t0 = time.perf_counter()
                    for (xs, ys, xe, ye, payload) in payloads:
                        disp.send_raw(payload, xs, ys, xe, ye)
                    times.append(time.perf_counter() - t0)
                med = statistics.median(times)
                print("%8d %10d %9.3f %9.1f %9.2f"
                      % (h, nbytes, nbytes / med / 1e6, 1 / med, med * 1e3))
    except pud_usb.PudError as exc:
        sys.exit(str(exc))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
