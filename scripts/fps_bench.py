#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
Full-screen / partial-refresh FPS benchmark (host side, end to end).

What is measured
----------------
Timing covers the whole submit path for one frame:

    EP0 control transfer (REQ_EP1_OUT window)  ->  EP1 bulk transfer

Frame *encoding* is done up front and deliberately excluded from the timing
loop, so the numbers describe the link and the device rather than python.
Because the firmware applies back-pressure on EP1 (it only arms the endpoint
when a decoder slot is free), dev.write() blocks until the device has
consumed the frame, so wall-clock time here really is end to end.

Reported per case:
  encode     median host encode time per frame (outside the timing loop)
  min        fastest frame period -- device idle, so approx. pure USB
  median     sustained frame period
  fps        frames / total wall time
  MB/s       compressed bytes / total wall time
  verdict    USB-bound or device-bound, from median / min

The device must NOT be bound to the pud kernel driver; see pud_usb.py for
the udev rule that avoids needing root.

Usage:
    ./scripts/fps_bench.py                    # every case
    ./scripts/fps_bench.py --full             # full-screen only
    ./scripts/fps_bench.py --partial          # partial only
    ./scripts/fps_bench.py --frames 200
    ./scripts/fps_bench.py --image assets/xfce.jpg
    ./scripts/fps_bench.py --window 64x64 --window 200x100
    ./scripts/fps_bench.py --pattern solid,noise
    ./scripts/fps_bench.py --dry-run          # no device needed

numpy builds and packs the test patterns; the QOI encoder, protocol and
device handling come from pud_usb, so there is only one encoder to maintain.
'''

import argparse
import os
import statistics
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pud_usb

XRES = 480
YRES = 320

BAND_PIXELS = pud_usb.PUD_MAX_BAND_PIXELS
TRANS_MAX = pud_usb.USB_TRANS_MAX_SIZE

#: Firmware frame slot: anything larger must be banded.
FRAME_SLOT = 65536

#: Distinct frames pre-encoded per case, cycled through the timing loop.
POOL = 3

#: Partial-refresh windows: (width, height), centred on the panel.
DEFAULT_WINDOWS = [(64, 64), (128, 64), (XRES, 8)]


# ---------------------------------------------------------------------------
# Test content
# ---------------------------------------------------------------------------

def make_solid(i):
    shade = (32 + i * 60) & 0xFF
    rgb = np.zeros((YRES, XRES, 3), np.uint8)
    rgb[:, :, 0] = shade
    rgb[:, :, 1] = 255 - shade
    rgb[:, :, 2] = 128
    return rgb


def make_gradient(i):
    xs = np.tile(np.arange(XRES, dtype=np.uint16)[None, :], (YRES, 1))
    ys = np.tile(np.arange(YRES, dtype=np.uint16)[:, None], (1, XRES))
    xs = (xs + i * 40) % XRES
    return np.stack([xs * 255 // XRES, ys * 255 // YRES,
                     (xs + ys) * 255 // (XRES + YRES)], -1).astype(np.uint8)


def make_noise(i):
    return np.random.default_rng(1000 + i).integers(
        0, 256, (YRES, XRES, 3), dtype=np.uint8)


def make_synth_photo(i):
    """Photo-like stand-in: sum of up-sampled random octaves."""
    rng = np.random.default_rng(2000 + i)
    acc = np.zeros((YRES, XRES, 3), np.float32)
    amp = 1.0
    for block in (64, 32, 16, 8, 4):
        low = rng.integers(0, 256, (YRES // block + 1, XRES // block + 1, 3))
        acc += np.kron(low.astype(np.float32),
                       np.ones((block, block, 1), np.float32))[:YRES, :XRES] * amp
        amp *= 0.55
    acc -= acc.min()
    acc = acc / max(acc.max(), 1.0) * 255.0
    return acc.astype(np.uint8)


PATTERNS = {"solid": make_solid, "gradient": make_gradient,
            "noise": make_noise}


def make_photo(i, image_path):
    """A real image if one loads, else a synthetic stand-in."""
    path = image_path or os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "assets", "xfce.jpg")
    try:
        raw = pud_usb.load_image(path, XRES, YRES, fit=False)
        img = np.frombuffer(raw, np.uint8).reshape(YRES, XRES, 3).copy()
        return np.roll(np.roll(img, i * 29, axis=1), i * 17, axis=0)
    except pud_usb.PudError:
        return make_synth_photo(i)


def pattern_rgb565(name, i, image_path):
    rgb = make_photo(i, image_path) if name == "photo" else PATTERNS[name](i)
    return pud_usb.rgb888_to_rgb565(rgb.tobytes(), XRES, YRES)


# ---------------------------------------------------------------------------
# Case construction
# ---------------------------------------------------------------------------

def band_rects(xs, ys, xe, ye):
    """Split like the driver's pud_fb_dirty() does: by pixel count."""
    rows = max(1, BAND_PIXELS // (xe - xs + 1))
    return [(xs, y, xe, min(y + rows - 1, ye))
            for y in range(ys, ye + 1, rows)]


def encode_region(rgb565, xs, ys, xe, ye):
    return pud_usb.qoi_encode(
        pud_usb.crop_rgb565(rgb565, XRES, xs, ys, xe - xs + 1, ye - ys + 1))


def build_full_case(pattern, image_path):
    """Full-screen frames.

    Returns (banded, single): the driver-style pixel-count banding, and --
    only when a whole frame fits one transfer -- the same frame encoded as
    ONE QOI stream, so the cost of the banding rule is visible. The single
    stream has to be a fresh full-frame encode, not the band streams joined:
    each transfer is decoded as a self-contained QOI image.
    """
    banded = []
    whole = []
    for i in range(POOL):
        px = pattern_rgb565(pattern, i, image_path)
        banded.append([(xs, ys, xe, ye, encode_region(px, xs, ys, xe, ye))
                       for (xs, ys, xe, ye) in
                       band_rects(0, 0, XRES - 1, YRES - 1)])
        whole.append([(0, 0, XRES - 1, YRES - 1, pud_usb.qoi_encode(px))])

    single = whole if all(len(f[0][4]) <= TRANS_MAX for f in whole) else None
    return banded, single


def build_partial_case(window, image_path):
    """Fixed-size centred window whose content changes every frame."""
    w, h = window
    xs, ys = (XRES - w) // 2, (YRES - h) // 2
    xe, ye = xs + w - 1, ys + h - 1

    frames = []
    for i in range(POOL):
        big = make_photo(i, image_path)
        rgb = np.roll(big, i * 13, axis=0)[ys:ye + 1, xs:xe + 1]
        frames.append([(xs, ys, xe, ye,
                        pud_usb.qoi_encode(
                            pud_usb.rgb888_to_rgb565(rgb.tobytes(), w, h)))])
    return frames


# ---------------------------------------------------------------------------
# Measurement / reporting
# ---------------------------------------------------------------------------

def measure(disp, frames, count):
    """Run the timing loop; return the list of per-frame periods in seconds."""
    periods = []
    for i in range(count):
        t0 = time.perf_counter()
        for (xs, ys, xe, ye, payload) in frames[i % len(frames)]:
            disp.send_raw(payload, xs, ys, xe, ye)
        periods.append(time.perf_counter() - t0)
    return periods


def report(label, note, periods, encode_ms, nbytes):
    total = sum(periods)
    ordered = sorted(periods)
    fast = ordered[0] * 1e3
    med = statistics.median(periods) * 1e3
    p90 = ordered[int(len(ordered) * 0.9) - 1] * 1e3
    slow = ordered[-1] * 1e3
    ratio = med / fast if fast > 0 else 0.0
    verdict = ("USB 受限 (steady/min = %.2fx)" % ratio if ratio < 1.3
               else "设备侧受限 (steady/min = %.2fx)" % ratio)

    print("=== %s ===" % label)
    if note:
        print("  %s" % note)
    print("  编码(主机, 不计入计时)  %.1f ms/帧" % encode_ms)
    print("  min(空闲态, ≈纯USB)     %.2f ms" % fast)
    print("  稳态                    %.2f ms  (p90 %.2f / max %.2f)"
          % (med, p90, slow))
    print("  帧率                    %.2f fps" % (len(periods) / total))
    print("  压缩吞吐                %.3f MB/s  (%.0f B/帧)"
          % (nbytes * len(periods) / total / 1e6, nbytes))
    print("  判定                    %s" % verdict)
    print()


def report_dry(label, note, encode_ms, frames, banded):
    """Print what a case would send, without touching the device."""
    sizes = [len(t[4]) for t in frames[0]]
    over = [s for s in sizes if s > TRANS_MAX]
    print("=== %s ===" % label)
    if note:
        print("  %s" % note)
    print("  编码(主机)              %.1f ms/帧" % encode_ms)
    print("  每帧段数                %d" % len(sizes))
    print("  单段字节  min/median/max  %d / %d / %d"
          % (min(sizes), int(statistics.median(sizes)), max(sizes)))
    print("  单段 ≤ %d B  (硬约束)   %s"
          % (TRANS_MAX, "✓" if not over else "✗ 超限!"))
    if banded:
        pxmax = max((t[2] - t[0] + 1) * (t[3] - t[1] + 1) for t in frames[0])
        print("  单段像素  max           %d  (驱动分带规则上限 %d)"
              % (pxmax, BAND_PIXELS))
    print()


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Full-screen / partial-refresh FPS benchmark")
    ap.add_argument("--frames", type=int, default=120,
                    help="frames per case (default 120)")
    ap.add_argument("--full", action="store_true", help="full-screen cases only")
    ap.add_argument("--partial", action="store_true",
                    help="partial-refresh cases only")
    ap.add_argument("--pattern", default="solid,gradient,photo,noise",
                    help="full-screen patterns (default all four)")
    ap.add_argument("--image", default=None,
                    help="image for the photo pattern (default assets/xfce.jpg)")
    ap.add_argument("--window", action="append", default=None, metavar="WxH",
                    help="extra partial-refresh window, repeatable")
    ap.add_argument("--dry-run", action="store_true",
                    help="build every case and print payload sizes only")
    args = ap.parse_args()

    want_full = args.full or not args.partial
    want_partial = args.partial or not args.full

    patterns = [p.strip() for p in args.pattern.split(",") if p.strip()]
    for p in patterns:
        if p != "photo" and p not in PATTERNS:
            sys.exit("unknown pattern %r" % p)

    windows = list(DEFAULT_WINDOWS)
    for spec in (args.window or []):
        try:
            w, h = (int(v) for v in spec.lower().split("x"))
        except ValueError:
            sys.exit("bad --window %r, expected WxH" % spec)
        if not (0 < w <= XRES and 0 < h <= YRES):
            sys.exit("window %dx%d out of range" % (w, h))
        windows.append((w, h))

    if args.image and not os.path.isfile(args.image):
        sys.exit("--image %s: no such file" % args.image)

    # Build every case up front: the timing loop must not include encoding,
    # so all payloads are prepared before any of them is sent.
    cases = []
    if want_full:
        for p in patterns:
            t0 = time.perf_counter()
            banded, single = build_full_case(p, args.image)
            enc = (time.perf_counter() - t0) * 1e3 / len(banded)
            n = sum(len(t[4]) for t in banded[0])
            note = "驱动分带: %d 段/帧, 压缩后 %d B/帧" % (len(banded[0]), n)
            if n > FRAME_SLOT:
                note += " (> 固件帧槽 %d B, 必须分带)" % FRAME_SLOT
            cases.append(("full", "full / %s" % p, note, banded, enc, n, True))
            if single:
                n1 = len(single[0][0][4])
                cases.append(("full", "full / %s (单次传输)" % p,
                              "不分带: 1 段/帧, 压缩后 %d B/帧" % n1,
                              single, enc, n1, False))
    if want_partial:
        for (w, h) in windows:
            t0 = time.perf_counter()
            frames = build_partial_case((w, h), args.image)
            enc = (time.perf_counter() - t0) * 1e3 / len(frames)
            cases.append(("partial", "partial / %dx%d" % (w, h),
                          "窗口居中, %d B/帧" % sum(len(t[4]) for t in frames[0]),
                          frames, enc, sum(len(t[4]) for t in frames[0]),
                          False))

    if args.dry_run:
        print("dry run: no device access\n")
        section = None
        for kind, label, note, frames, enc, _n, banded in cases:
            if kind != section:
                section = kind
                print("########## %s ##########\n"
                      % ("全刷 (full-screen %dx%d)" % (XRES, YRES)
                         if kind == "full" else "局刷 (partial refresh)"))
            report_dry(label, note, enc, frames, banded)
        return

    try:
        with pud_usb.open_device() as disp:
            print("device %04x:%04x claimed; frames/case = %d\n"
                  % (pud_usb.VID, pud_usb.PID, args.frames))

            section = None
            for kind, label, note, frames, enc, n, _banded in cases:
                if kind != section:
                    section = kind
                    print("########## %s ##########\n"
                          % ("全刷 (full-screen %dx%d)" % (XRES, YRES)
                             if kind == "full" else "局刷 (partial refresh)"))
                report(label, note, measure(disp, frames, args.frames), enc, n)
                time.sleep(0.3)
    except pud_usb.PudError as exc:
        sys.exit(str(exc))
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
