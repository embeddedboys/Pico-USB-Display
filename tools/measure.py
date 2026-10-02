#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
Reusable measurement primitives: build a deterministic payload set, send it
N times, report median/min/max and throughput.

This module only produces facts.  It never decides whether a number is good:
`median`, `min`, `max`, `mb_per_s` and `payload_bytes` are observations, and
the test that calls it owns the oracle (see `tests/common/harness.py`).
Keeping the timer loop here is what lets `tests/test_ep1_throughput.py`,
`tools/pudctl.py meter` and `tools/fps_bench.py` share one implementation
instead of three subtly different ones.

Conventions that matter:

- **Encoding is never inside the timed loop.**  Hand-written Python QOI/RLE
  encoders cost tens of milliseconds per frame (measured 43.8 / 54.8 ms for a
  full 480x320 frame) and would measure the interpreter, not the device.  The
  caller pre-encodes; `meter()` only calls `Display.send_raw()`.
- **Statistics are explicit**: median plus min/max as the dispersion measure,
  not a single sample.

    from measure import deterministic_rgb565, band_rects, meter
'''

import statistics
import time

#: Full-speed USB is 12 Mbit/s.  A bulk endpoint gets at most one 64-byte
#: packet per 1 ms frame, i.e. 19 x 64 B per 1 ms as the widely quoted ceiling
#: => 1.216 MB/s.  The device datasheet/USB 2.0 spec is the source; the number
#: is used as an upper bound for what a link can produce, never as a target.
#: Measured on this hardware: 1.130 MB/s on a direct xHCI root port (93% of
#: it), 0.83 MB/s behind a 480M hub's TT.  See notes/scripts.md.
USB_FS_THEORETICAL_MBPS = 1.216


def deterministic_rgb565(width, height, seed=12345):
    """Incompressible-enough RGB565 bytes: payload tracks the window size.

    A plain LCG, so the same call always produces the same bytes without
    numpy and without a random seed that drifts between runs.
    """
    n = width * height * 3
    rgba = bytearray(n)
    for i in range(n):
        seed = (seed * 1103515245 + 12345) & 0xFFFFFFFF
        rgba[i] = (seed >> 16) & 0xFF
    import pud_usb

    return pud_usb.rgb888_to_rgb565(bytes(rgba), width, height)


def band_rects(band_pixels, width, height):
    """Split a full-width rectangle the way the driver does.

    Rows per band come from the device-reported `band_pixels`, so the same
    code works on RP2350 (64 KB transfers) and RP2040 (32 KB).  Returns
    inclusive (xs, ys, xe, ye) tuples, matching the EP1 header.
    """
    rows = max(1, band_pixels // width)
    return [(0, y, width - 1, min(y + rows - 1, height - 1))
            for y in range(0, height, rows)]


def encode_bands(rgb565, width, rects, codec="qoi"):
    """Pre-encode every band outside the timer.  Returns [(rect, payload)]."""
    import pud_usb

    out = []
    for (xs, ys, xe, ye) in rects:
        patch = pud_usb.crop_rgb565(rgb565, width, xs, ys,
                                    xe - xs + 1, ye - ys + 1)
        out.append(((xs, ys, xe, ye), pud_usb.ENCODERS[codec](patch)))
    return out


def meter(display, bands, repeats=20, gap_ms=0.0, timeout=None):
    """Send `bands` (a list of (rect, payload)) `repeats` times, timing each.

    One pass over all bands is one measurement, exactly like a driver frame.
    Returns an observation dict; no verdict, no threshold.
    """
    times = []
    payload_bytes = sum(len(p) for _, p in bands)
    for _ in range(repeats):
        t0 = time.perf_counter()
        for (xs, ys, xe, ye), payload in bands:
            display.send_raw(payload, xs, ys, xe, ye, timeout=timeout)
        times.append(time.perf_counter() - t0)
        if gap_ms:
            time.sleep(gap_ms / 1e3)

    med = statistics.median(times)
    return {
        "repeats": repeats,
        "gap_ms": gap_ms,
        "payload_bytes": payload_bytes,
        "median_s": med,
        "min_s": min(times),
        "max_s": max(times),
        "mb_per_s": payload_bytes / med / 1e6,
        "fps": 1.0 / med,
        "samples_s": times,
    }


def summarize(rows, fields):
    """Render a list of dicts as a small text table (for humans only)."""
    widths = [max(len(str(f)), *(len(str(r.get(f, ""))) for r in rows))
              for f in fields]
    out = ["  ".join(f.ljust(w) for f, w in zip(fields, widths))]
    out.append("  ".join("-" * w for w in widths))
    for r in rows:
        out.append("  ".join(str(r.get(f, "")).ljust(w)
                             for f, w in zip(fields, widths)))
    return "\n".join(out)
