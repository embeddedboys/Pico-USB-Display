#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# Copyright (c) 2020 2024 Daniel Gorbea
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
Play a video on the Pico USB Display.

Frames are decoded by ffmpeg through a rawvideo pipe and re-compressed as
RGB565 QOI on the fly, so nothing is written to disk and no python video
binding is needed. Requires firmware built with DECODER_TYPE=3 (QOI).

Note the two limits this script cannot beat:
  * full-screen QOI of photo-like content is around 120 KB, and the link
    carries about 1.0 MB/s, so expect roughly 8 fps for real video;
  * the host has to encode every frame in python, which costs tens of ms.
    Passing --fps below that keeps playback smooth instead of dropped.

Usage:
    ./scripts/video_player.py [options] <video>

Options:
    --xres W, --yres H   panel size (default 480x320)
    --fps N              cap the playback rate (default: as fast as possible)
    --frames N           stop after N frames
    --no-loop            play once instead of looping
    --stretch            stretch to the panel instead of letterboxing
    --stats              print a timing line every second

Examples:
    ./scripts/video_player.py --fps 8 ~/Videos/jazz.mp4
    ./scripts/video_player.py --frames 100 --stats test.mp4
'''

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pud_usb


def main():
    ap = argparse.ArgumentParser(description="play a video on the panel")
    ap.add_argument("video")
    ap.add_argument("--xres", type=int, default=480)
    ap.add_argument("--yres", type=int, default=320)
    ap.add_argument("--fps", type=float, default=None,
                    help="cap the playback rate")
    ap.add_argument("--frames", type=int, default=None,
                    help="stop after this many frames")
    ap.add_argument("--no-loop", action="store_true")
    ap.add_argument("--stretch", action="store_true")
    ap.add_argument("--stats", action="store_true")
    args = ap.parse_args()

    interval = 1.0 / args.fps if args.fps else 0.0
    loop = not args.no_loop

    try:
        with pud_usb.open_device() as disp:
            disp.width, disp.height = args.xres, args.yres
            played = 0
            t_start = time.perf_counter()
            t_report = t_start
            bytes_sent = 0

            while True:
                for raw in pud_usb.video_frames(args.video, args.xres,
                                                args.yres,
                                                fit=not args.stretch):
                    t0 = time.perf_counter()
                    rgb565 = pud_usb.rgb888_to_rgb565(raw, args.xres, args.yres)
                    _, nbytes, _ = disp.send_full(rgb565)
                    bytes_sent += nbytes
                    played += 1

                    if interval:
                        slack = interval - (time.perf_counter() - t0)
                        if slack > 0:
                            time.sleep(slack)

                    if args.stats and time.perf_counter() - t_report >= 1.0:
                        el = time.perf_counter() - t_report
                        print("%d frames, %.1f fps, %.2f MB/s"
                              % (played, played / (time.perf_counter()
                                                   - t_start),
                                 bytes_sent / (time.perf_counter()
                                               - t_start) / 1e6))
                        t_report = time.perf_counter()

                    if args.frames and played >= args.frames:
                        break

                if not loop or (args.frames and played >= args.frames):
                    break

            el = time.perf_counter() - t_start
            print("%d frames in %.1f s -> %.1f fps, %.2f MB/s"
                  % (played, el, played / el if el else 0,
                     bytes_sent / el / 1e6 if el else 0))
    except pud_usb.PudError as exc:
        sys.exit(str(exc))
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
