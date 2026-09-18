#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
Read EP4 touch reports from userspace, without the kernel driver.

The firmware polls the touch controller every `tp.polling_period` ms and pushes
one 8-byte report per poll while the panel is held, plus one on release:

    flags(bit0 pressed), x>>8, x&0xff, y>>8, y&0xff, seq, version, reserved

so the host only has to keep reading EP4.  `--mode poll` uses REQ_EP4_IN
instead (ask for one report, then read it), which is what the first version of
the kernel driver did; comparing the two shows whether the handshake is the
problem or the reports themselves.

    ./scripts/touch_test.py                 # push mode, 10 s
    ./scripts/touch_test.py --mode poll     # request/read handshake
    ./scripts/touch_test.py --calibrate     # touch all four corners

Exit status is non-zero if EP4 never reports a version (touch not implemented in
the running firmware) or a report fails to parse.
'''

import argparse
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pud_usb


def wall_clock():
    return time.strftime("%H:%M:%S")


def main():
    ap = argparse.ArgumentParser(description="EP4 touch input test")
    ap.add_argument("--mode", choices=("push", "poll"), default="push",
                    help="push: just read EP4 (default); poll: REQ_EP4_IN first")
    ap.add_argument("--seconds", type=float, default=10.0,
                    help="how long to listen (default 10 s)")
    ap.add_argument("--timeout", type=float, default=500.0,
                    help="timeout for one report, ms (default 500)")
    ap.add_argument("--idle-timeout", type=float, default=200.0,
                    help="poll mode: how long to wait for a report, ms")
    ap.add_argument("--calibrate", action="store_true",
                    help="collect the bounding box and check the axis mapping")
    ap.add_argument("--quiet", action="store_true",
                    help="only print the summary")
    args = ap.parse_args()

    reports = 0
    presses = releases = 0
    seqs = []
    xs, ys = [], []
    gaps = []
    last_t = None
    last_pressed = False
    version = None
    errors = 0

    try:
        with pud_usb.open_device() as disp:
            print("device caps=%s  mode=%s  listening %.1f s"
                  % (disp.caps, args.mode, args.seconds))
            if args.calibrate:
                print("touch each corner of the panel slowly, in any order")
            print("%-8s %-7s %-5s %-5s %-4s %-8s" %
                  ("time", "state", "x", "y", "seq", "dt"))

            deadline = time.perf_counter() + args.seconds
            while time.perf_counter() < deadline:
                try:
                    if args.mode == "poll":
                        disp.touch_request()
                    rep = disp.read_touch(timeout=int(args.timeout))
                except pud_usb.PudError as exc:
                    sys.exit(str(exc))
                except Exception as exc:            # usb.core.USBTimeoutError
                    if type(exc).__name__ != "USBTimeoutError":
                        raise
                    errors += 1
                    if args.mode == "poll":
                        time.sleep(args.idle_timeout / 1e3)
                    continue

                now = time.perf_counter()
                if last_t is not None:
                    gaps.append((now - last_t) * 1e3)
                last_t = now

                reports += 1
                seqs.append(rep["seq"])
                if rep["version"]:
                    version = rep["version"]
                if rep["pressed"]:
                    presses += 1
                    xs.append(rep["x"])
                    ys.append(rep["y"])
                elif last_pressed:
                    releases += 1
                last_pressed = rep["pressed"]

                if not args.quiet:
                    print("%-8s %-7s %-5d %-5d %-4d %-8s"
                          % (wall_clock(),
                             "PRESS" if rep["pressed"] else "release",
                             rep["x"], rep["y"], rep["seq"],
                             "%.1f" % gaps[-1] if gaps else "-"))

                if args.mode == "poll":
                    time.sleep(args.idle_timeout / 1e3)

    except pud_usb.PudError as exc:
        sys.exit(str(exc))
    except KeyboardInterrupt:
        pass

    print()
    print("reports    : %d (%d pressed, %d releases, %d timeouts)"
          % (reports, presses, releases, errors))
    if version is None:
        print("version    : none -- this firmware does not report touch "
              "(EP4 is still a stub)")
        return 1
    print("version    : %d" % version)
    if xs:
        print("x range    : %d..%d" % (min(xs), max(xs)))
        print("y range    : %d..%d" % (min(ys), max(ys)))
    if gaps:
        print("report gap : median %.1f ms, min %.1f, max %.1f"
              % (statistics.median(gaps), min(gaps), max(gaps)))
    if len(seqs) > 1:
        if args.mode == "poll":
            # a polling host gets the *current* report, so the sequence only
            # moves when the device has something new; repeats are expected
            print("sequence   : %d..%d, %d distinct (repeats are expected in "
                  "poll mode)" % (seqs[0], seqs[-1], len(set(seqs))))
        else:
            # pushes only, so a jump means reports the host never saw
            missed = sum((b - a - 1) % 256 for a, b in zip(seqs, seqs[1:]))
            print("sequence   : %d..%d, %d missed/coalesced"
                  % (seqs[0], seqs[-1], missed))
    if args.calibrate and xs:
        span_x, span_y = max(xs) - min(xs), max(ys) - min(ys)
        print("calibration: touched box %dx%d at (%d,%d)"
              % (span_x, span_y, min(xs), min(ys)))
        if span_x > 380 and span_y > 250:
            print("             looks right (x spans the 480 wide panel, "
                  "y the 320 tall one)")
        elif span_y > 380 and span_x > 250:
            print("             x/y look SWAPPED -- check indev_set_dir()")
        else:
            print("             box smaller than the panel: not all corners "
                  "were touched")

    return 0


if __name__ == "__main__":
    sys.exit(main())
