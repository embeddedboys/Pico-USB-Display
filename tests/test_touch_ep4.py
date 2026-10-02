#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
EP4 touch reports: the device pushes them, the host reads them, and every
field stays inside the contract.

The firmware polls the touch controller every `tp.polling_period` ms and pushes
one 8-byte report per poll while the panel is held, plus one on release:

    flags(bit0 pressed), x>>8, x&0xff, y>>8, y&0xff, seq, version, reserved

`--mode poll` uses `REQ_EP4_IN` instead (ask for one report, then read it),
which is what the first version of the kernel driver did; comparing the two
shows whether a problem is the handshake or the reports themselves.

    python3 tests/test_touch_ep4.py                     # push, 10 s
    python3 tests/test_touch_ep4.py --mode poll         # request/read handshake
    python3 tests/test_touch_ep4.py --calibrate         # touch each corner

Expectation, and why some runs are not PASS/FAIL:

  - the report parses, `version` is 1 (the value `PUD_TOUCH_VERSION` defines),
    and every accepted coordinate is inside the geometry the capability report
    advertises -- all three are SPEC items;
  - whether reports arrive at all depends on a human touching the panel.  An
    idle panel producing zero reports is *expected* (the device stays silent),
    so that run is INCONCLUSIVE, not FAIL and not PASS.  The earlier version of
    this script called it "firmware does not report touch", which was a false
    negative on an idle panel.
  - the device-side `--calibrate` bounding box is an observation for a human to
    read; a coordinate outside the reported geometry *is* a failure, but a small
    box just means not every corner was touched.

Needs the device with the `pud` kernel driver unbound.  No device =>
ENVIRONMENT_ERROR (3), never FAIL.
'''

import os
import statistics
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "common"))
sys.path.insert(0, os.path.join(_HERE, "tools"))

from harness import Oracle, Report, Snapshot, Test, run_test  # noqa: E402
import pud_usb  # noqa: E402

# ORACLE: SPEC
# SOURCE: notes/usb-protocol.md, "EP4 触摸的处理" -- the 8-byte layout table,
#         version = PUD_TOUCH_VERSION (1), "coordinates are panel coordinates
#         in the display coordinate system (0..xres-1 / 0..yres-1), already
#         transformed and clamped by the firmware", and "the device sends
#         nothing while idle" for the zero-report case
# EXPECTED: parses; version == 1; 0 <= x < xres and 0 <= y < yres; sequence
#           advances monotonically with wrap; zero reports on an idle panel is
#           expected and is reported as INCONCLUSIVE
ORACLE = Oracle(
    "SPEC",
    "notes/usb-protocol.md (EP4 report layout, PUD_TOUCH_VERSION = 1, "
    "coordinate range and clamping, idle-is-silent)",
    "report parses with version 1 and coordinates inside the reported "
    "geometry; sequence advances; an idle panel legitimately yields no reports")

TOUCH_VERSION = 1


def extra_args(ap):
    ap.add_argument("--mode", choices=("push", "poll"), default="push",
                    help="push: just read EP4 (default); poll: REQ_EP4_IN first")
    ap.add_argument("--seconds", type=float, default=10.0,
                    help="how long to listen (default 10 s)")
    ap.add_argument("--idle-timeout", type=float, default=200.0,
                    help="poll mode: ms to wait after each request")
    ap.add_argument("--calibrate", action="store_true",
                    help="report the touched bounding box and span")
    ap.add_argument("--rate", type=float, default=0.0,
                    help="poll mode: minimum ms between requests")


def check(dev, args):
    snapped = dev.caps or {}
    xres = snapped.get("xres") or 480
    yres = snapped.get("yres") or 320
    has_touch = snapped.get("touch")

    snaps, failures = [], []
    reports, errors = 0, 0
    xs, ys, seqs, gaps = [], [], [], []
    version = None
    last_t = None

    deadline = time.perf_counter() + args.seconds
    while time.perf_counter() < deadline:
        try:
            if args.mode == "poll":
                dev.touch_request()
            rep = dev.read_touch(timeout=500)
        except Exception as exc:
            if type(exc).__name__ != "USBTimeoutError":
                raise
            errors += 1
            if args.mode == "poll":
                time.sleep(max(args.idle_timeout, args.rate) / 1e3)
            continue

        now = time.perf_counter()
        if last_t is not None:
            gaps.append((now - last_t) * 1e3)
        last_t = now

        reports += 1
        seqs.append(rep["seq"])
        if rep["version"] != TOUCH_VERSION:
            failures.append("report %d has version %d, spec says %d"
                            % (reports, rep["version"], TOUCH_VERSION))
        version = rep["version"]
        if rep["pressed"]:
            x, y = rep["x"], rep["y"]
            xs.append(x)
            ys.append(y)
            if not (0 <= x < xres and 0 <= y < yres):
                failures.append("press %d at (%d,%d) is outside the reported "
                                "geometry %dx%d" % (reports, x, y, xres, yres))
        if args.mode == "poll" and args.rate:
            time.sleep(args.rate / 1e3)

    snaps.append(Snapshot("mode", args.mode))
    snaps.append(Snapshot("caps_touch_bit", has_touch))
    snaps.append(Snapshot("geometry", "%dx%d" % (xres, yres)))
    snaps.append(Snapshot("reports", reports))
    snaps.append(Snapshot("timeouts", errors))
    if version is not None:
        snaps.append(Snapshot("version", version))
    if xs:
        snaps.append(Snapshot("x_range", "%d..%d" % (min(xs), max(xs))))
        snaps.append(Snapshot("y_range", "%d..%d" % (min(ys), max(ys))))
    if gaps:
        snaps.append(Snapshot("report_gap_ms",
                              "median %.1f, min %.1f, max %.1f"
                              % (statistics.median(gaps), min(gaps),
                                 max(gaps))))
    if len(seqs) > 1:
        if args.mode == "poll":
            snaps.append(Snapshot("sequence",
                                  "%d..%d, %d distinct (repeats are expected in "
                                  "poll mode)" % (seqs[0], seqs[-1],
                                                  len(set(seqs)))))
        else:
            missed = sum((b - a - 1) % 256 for a, b in zip(seqs, seqs[1:]))
            snaps.append(Snapshot("sequence_missed_or_coalesced", missed,
                                  "samples"))
    if args.calibrate and xs:
        span_x, span_y = max(xs) - min(xs), max(ys) - min(ys)
        snaps.append(Snapshot("touched_box",
                              "%dx%d at (%d,%d)"
                              % (span_x, span_y, min(xs), min(ys))))
        if span_x > 380 and span_y > 250:
            note = "span looks like the full panel"
        elif span_y > 380 and span_x > 250:
            note = "x/y look SWAPPED -- check indev_set_dir()"
        else:
            note = "box smaller than the panel: not all corners were touched"
        snaps.append(Snapshot("calibration_note", note))

    if failures:
        return Report.of(snaps, ok=False, detail="; ".join(failures[:5]))
    if reports == 0:
        reason = ("the capability report says this firmware has no touch "
                  "controller" if not has_touch
                  else "no touch was observed in %.1fs" % args.seconds)
        return Report.of(
            snaps, inconclusive=True,
            detail="%s.  The device stays silent while idle, so this is not a "
                   "failure -- touch the panel and rerun to get a verdict."
                   % reason)
    return Report.of(snaps, ok=True,
                     detail="%d report(s), all inside the advertised geometry"
                            % reports)


TEST = Test(
    "test_touch_ep4", ORACLE, check, default_timeout=120.0,
    description="EP4 touch report contract (needs device + a finger)")
TEST.extra_args = extra_args

if __name__ == "__main__":
    run_test(TEST)
