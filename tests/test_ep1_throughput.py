#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
EP1 throughput: how many bytes per second the link and this endpoint can carry,
and whether the rate depends on the payload size.

This probe sends *real, decodable* QOI streams (a junk-payload version measured
nothing -- the decoder rejects it) and sweeps the band height.  It exists to
separate two failure shapes:

  - **rate flat across sizes**  => the link is the limit, and device-side decode
    is hidden behind it.  This is the normal state on full-speed USB.
  - **rate falls as payload grows** => the device is not keeping up.

The interpretation is printed but is not a verdict; see the oracle below for
what this test is allowed to fail on.

    python3 tests/test_ep1_throughput.py
    python3 tests/test_ep1_throughput.py --frames 60 --json
    pudctl meter --repeats 50          # the same measurement, as a tool

Needs the device with the `pud` kernel driver unbound.  Encoding happens once,
outside the timed loop: hand-written Python QOI costs tens of milliseconds per
frame and would otherwise be what gets measured.
'''

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "common"))
sys.path.insert(0, os.path.join(_HERE, "tools"))

from harness import (Oracle, Report, Snapshot, Test,  # noqa: E402
                     Inconclusive, run_test)
import measure  # noqa: E402

# USB 2.0 full-speed is 12 Mbit/s.  A bulk endpoint can carry at most one
# 64-byte packet per 1 ms frame, and 19 packets fit per 1 ms frame
# (19 x 64 B x 1000 /s = 1.216 MB/s), which is the standard's ceiling for this
# endpoint.  Nothing in this project can exceed it, so a number above it means
# the measurement is wrong (a short timing loop, or a cached reply), not that
# the hardware got faster.
#
# A *lower* bound is deliberately NOT asserted: no specification in this repo
# states a minimum EP1 throughput.  The repo's measured range on a direct root
# port is 1.11-1.13 MB/s and 0.82-0.83 MB/s behind a hub's TT (notes/scripts.md),
# i.e. topology-dependent -- so a throughput floor would be an invented
# criterion, and the dispersion comment printed below is the observation a
# reader should use instead.
#
# ORACLE: SPEC
# SOURCE: USB 2.0 full-speed bulk ceiling (19 x 64 B per 1 ms frame);
#         topology context from notes/scripts.md
# EXPECTED: measured throughput <= 1.216 MB/s
ORACLE = Oracle(
    "SPEC",
    "USB 2.0 full-speed bulk transfer ceiling for a 64-byte-MPS endpoint "
    "(19 x 64 B / 1 ms); notes/scripts.md for the measured root-port and "
    "hub-TT figures this run is compared against",
    "no sweep height may exceed 1.216 MB/s (a physical bound; exceeding it "
    "means the measurement is invalid)",
    "no throughput floor is asserted -- none is specified in this repo")

HEIGHTS = (8, 16, 32, 64, 128, 320)


def extra_args(ap):
    ap.add_argument("--frames", type=int, default=40,
                    help="timed passes per height (default 40)")
    ap.add_argument("--heights", type=int, nargs="+", default=list(HEIGHTS),
                    help="band heights to sweep (default %s)" % (HEIGHTS,))
    ap.add_argument("--xres", type=int, default=None,
                    help="override the panel width (default: what the device "
                         "reports)")
    ap.add_argument("--yres", type=int, default=None,
                    help="override the panel height (default: device)")
    ap.add_argument("--codec", default="qoi", choices=["qoi", "rle"],
                    help="payload codec (must match the firmware's "
                         "DECODER_TYPE; default qoi)")


def check(dev, args):
    caps = dev.caps or {}
    xres = args.xres or caps.get("xres") or 480
    yres = args.yres or caps.get("yres") or 320
    snaps, failures, rows = [], [], []

    for height in args.heights:
        if height > yres:
            continue
        rgb565 = measure.deterministic_rgb565(xres, height)
        rects = measure.band_rects(dev.band_pixels, xres, height)
        bands = measure.encode_bands(rgb565, xres, rects, codec=args.codec)
        r = measure.meter(dev, bands, repeats=args.frames)
        r["height"] = height
        rows.append(r)
        snaps.append(Snapshot("height_%d" % height,
                              "%d B in %.3f ms median, %.3f MB/s"
                              % (r["payload_bytes"], r["median_s"] * 1e3,
                                 r["mb_per_s"])))

    if not rows:
        raise Inconclusive("no sweep height fits a %dx%d panel" % (xres, yres))

    worst = max(rows, key=lambda r: r["mb_per_s"])
    best = max(rows, key=lambda r: r["mb_per_s"])
    snaps.append(Snapshot("topology_speed_mb_per_s",
                          round(worst["mb_per_s"], 3), "MB/s"))
    snaps.append(Snapshot("ceiling_mb_per_s",
                          measure.USB_FS_THEORETICAL_MBPS, "MB/s"))
    rates = [r["mb_per_s"] for r in rows]
    snaps.append(Snapshot("dispersion_across_heights",
                          "%.3f..%.3f MB/s (spread %.1f%%)"
                          % (min(rates), max(rates),
                             100.0 * (max(rates) - min(rates)) / max(rates))))

    if worst["mb_per_s"] > measure.USB_FS_THEORETICAL_MBPS:
        failures.append("%.3f MB/s exceeds the full-speed bulk ceiling %.3f "
                        "MB/s -- the measurement is invalid, not the hardware "
                        "(check that the payload is not cached and that the "
                        "timer wraps the whole sweep)"
                        % (worst["mb_per_s"],
                           measure.USB_FS_THEORETICAL_MBPS))

    flat = (max(rates) - min(rates)) / max(rates) < 0.10
    interpretation = ("rate is flat across payload sizes => link-limited "
                      "(the normal state)" if flat else
                      "rate falls as payload grows => the device may not be "
                      "keeping up; compare against DECODER_STATS draw_us")
    detail = "%s; %s" % ("PASS" if not failures else "see failures",
                         interpretation)
    return Report.of(snaps, ok=not failures, detail=detail)


TEST = Test(
    "test_ep1_throughput", ORACLE, check,
    description="EP1 throughput sweep on real QOI payloads (needs device)",
    default_timeout=300.0)
TEST.extra_args = extra_args

if __name__ == "__main__":
    run_test(TEST)
