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

# USB 2.0 defines a bulk ceiling per speed, and both come from the standard's
# own framing rather than from any measurement:
#
#   full-speed : 19 x 64 B per 1 ms frame            = 1.216 MB/s
#   high-speed : 13 x 512 B per 125 us microframe    = 53.248 MB/s
#
# Which one applies is a property of the *link*, so it is chosen from the
# negotiated speed.  Asserting the full-speed bound against a high-speed device
# reports a failure for working hardware -- the number is not suspicious, the
# oracle is.  Nothing in this project can exceed the applicable ceiling, so a
# number above it means the measurement is wrong (a short timing loop, or a
# cached reply), not that the hardware got faster.
#
# A *lower* bound is deliberately NOT asserted: no specification in this repo
# states a minimum EP1 throughput.  The repo's measured range on a direct root
# port is 1.11-1.13 MB/s and 0.82-0.83 MB/s behind a hub's TT (notes/scripts.md),
# i.e. topology-dependent -- so a throughput floor would be an invented
# criterion, and the dispersion comment printed below is the observation a
# reader should use instead.
#
# ORACLE: SPEC
# SOURCE: USB 2.0 bulk ceiling for the negotiated speed -- 19 x 64 B per 1 ms
#         frame at full-speed, 13 x 512 B per 125 us microframe at high-speed;
#         topology context from notes/scripts.md
# EXPECTED: measured throughput <= the ceiling for this link's speed
#           (1.216 MB/s full-speed / 53.248 MB/s high-speed)
ORACLE = Oracle(
    "SPEC",
    "USB 2.0 bulk transfer ceiling for the endpoint's speed: 19 x 64 B / 1 ms "
    "(full-speed, MPS 64) or 13 x 512 B / 125 us (high-speed, MPS 512); "
    "notes/scripts.md for the measured root-port and hub-TT figures",
    "no sweep height may exceed the ceiling of the negotiated speed (a "
    "physical bound; exceeding it means the measurement is invalid)",
    "no throughput floor is asserted -- none is specified in this repo")

# 13 x 512 B per 125 us microframe.
HS_BULK_CEILING_MBPS = 13 * 512 * 8000 / 1e6


def bulk_ceiling_mbps(dev):
    """Physical EP1 ceiling for the link actually in use.

    pyusb reports the negotiated speed as an integer (3 = high-speed).  A
    device that reports nothing is treated as full-speed, which is the
    conservative choice: it keeps the tighter bound when unsure.
    """
    usb_dev = getattr(dev, "dev", dev)
    return (HS_BULK_CEILING_MBPS if getattr(usb_dev, "speed", None) == 3
            else measure.USB_FS_THEORETICAL_MBPS)

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
    ceiling = bulk_ceiling_mbps(dev)
    snaps.append(Snapshot("ceiling_mb_per_s", ceiling,
                          "MB/s (negotiated speed %s)"
                          % (getattr(getattr(dev, "dev", dev), "speed", "?"),)))
    rates = [r["mb_per_s"] for r in rows]
    snaps.append(Snapshot("dispersion_across_heights",
                          "%.3f..%.3f MB/s (spread %.1f%%)"
                          % (min(rates), max(rates),
                             100.0 * (max(rates) - min(rates)) / max(rates))))

    if worst["mb_per_s"] > ceiling:
        failures.append("%.3f MB/s exceeds the bulk ceiling %.3f MB/s for this "
                        "link -- the measurement is invalid, not the hardware "
                        "(check that the payload is not cached and that the "
                        "timer wraps the whole sweep)"
                        % (worst["mb_per_s"], ceiling))

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
