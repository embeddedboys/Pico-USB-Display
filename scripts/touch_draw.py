#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
Touch feedback on the panel itself, from userspace (no kernel driver).

Reading the coordinates as numbers tells you they exist; drawing them tells you
whether they are *right*.  This paints a canvas on the panel and marks every
report where the device says you touched, so an axis swap, an inversion, a
rotation that does not follow the display or an offset from the panel lamination
is immediately visible instead of being a table of numbers.

    ./scripts/touch_draw.py                  # trace: the mark follows your finger
    ./scripts/touch_draw.py --mode grid      # reference grid + the mark
    ./scripts/touch_draw.py --mode targets   # touch 5 crosshairs, get the error

Modes:
    trace    a trail of dots where you touched (the default)
    grid     40 px grid with x/y labels, mark on top -- read the offset off the
             screen
    targets  five crosshairs (four corners, one centre); touch each one in turn
             and it prints the per-target error plus the offset/scale that would
             cancel it

The firmware must be built for the codec you send with (--codec, default qoi =
DECODER_TYPE 3) and must implement touch (the report's version != 0).  Touch is
pushed by the device, so this only reads EP4; an idle panel sends nothing, which
is why the read times out and the loop just carries on.

Ctrl-C to stop (the panel keeps the last canvas).
'''

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pud_usb

W, H = 480, 320

#: where the calibration crosshairs go, a little inside the edges because a
#: finger cannot reliably hit the outermost pixels
TARGETS = [(24, 24), (W - 25, 24), (W - 25, H - 25), (24, H - 25),
           (W // 2, H // 2)]

CURSOR = [(0, 0, 255), (0, 255, 0), (255, 255, 0), (255, 0, 255)]


def load_font(size=10):
    try:
        from PIL import ImageFont
        return ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", size)
    except Exception:
        return None


def make_canvas(mode):
    """A PIL RGB canvas, black except for the optional reference grid."""
    from PIL import Image, ImageDraw

    img = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(img)

    if mode == "grid":
        for x in range(0, W, 40):
            d.line([(x, 0), (x, H - 1)], fill=(32, 40, 48))
        for y in range(0, H, 40):
            d.line([(0, y), (W - 1, y)], fill=(32, 40, 48))
        font = load_font(10)
        for x in range(0, W, 80):
            d.text((x + 3, 3), str(x), fill=(110, 130, 150), font=font)
        for y in range(0, H, 80):
            d.text((3, y + 3), str(y), fill=(110, 130, 150), font=font)
        d.text((W // 2 - 60, H // 2 - 6), "touch anywhere", fill=(110, 130, 150),
               font=font)

    return img


def send_rect(disp, img, box, codec, stats):
    """Push one rectangle of the canvas to the panel."""
    x0, y0, x1, y1 = box
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(W - 1, x1), min(H - 1, y1)
    if x1 < x0 or y1 < y0:
        return

    w, h = x1 - x0 + 1, y1 - y0 + 1
    raw = img.crop((x0, y0, x1 + 1, y1 + 1)).tobytes()
    payload = pud_usb.rgb888_to_rgb565(raw, w, h)
    try:
        _, nbytes, secs = disp.send_rgb565(payload, w, h, x0, y0, codec=codec)
    except pud_usb.PudError as exc:
        print("  ! %s" % exc)
        return

    stats["rects"] += 1
    stats["bytes"] += nbytes
    stats["us"] += secs * 1e6


def crosshair(draw, x, y, colour, size=7, width=1):
    draw.line([(x - size, y), (x + size, y)], fill=colour, width=width)
    draw.line([(x, y - size), (x, y + size)], fill=colour, width=width)


def target_mark(draw, x, y, label):
    draw.ellipse([x - 10, y - 10, x + 10, y + 10], outline=(0, 120, 200))
    crosshair(draw, x, y, (0, 120, 200), size=6)
    draw.text((x + 12, y - 6), label, fill=(0, 120, 200), font=load_font(10))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=("trace", "grid", "targets"),
                    default="trace")
    ap.add_argument("--codec", default="qoi", choices=sorted(pud_usb.ENCODERS),
                    help="what the firmware is built for (DECODER_TYPE)")
    ap.add_argument("--timeout", type=float, default=500.0,
                    help="timeout for one report, ms (default 500)")
    ap.add_argument("--seconds", type=float, default=None,
                    help="stop after this long (default: until Ctrl-C)")
    ap.add_argument("--width", type=int, default=3,
                    help="marker size in pixels (default 3)")
    ap.add_argument("--rate", type=float, default=20.0,
                    help="max panel updates per second (default 20); reports "
                         "arriving faster are coalesced into the next update")
    ap.add_argument("--verbose", action="store_true",
                    help="print every report as it is drawn (for logging)")
    ap.add_argument("--gap-ms", type=float, default=0.0,
                    help="pause between panel updates (default 0): the link "
                         "was measured to survive back-to-back small transfers")
    args = ap.parse_args()

    from PIL import ImageDraw

    stats = {"rects": 0, "bytes": 0, "us": 0.0, "coalesced": 0}
    reports = 0
    misses = 0
    last = None

    # A touch report arrives every 33 ms (30/s), which is faster than a panel
    # update on this link, so updates are rate limited and the dirty rectangle
    # is coalesced: the canvas always has the newest marks, only the transfer
    # is deferred.
    interval = 1.0 / max(1.0, args.rate)
    pending = {"box": None, "last": 0.0}

    def mark_dirty(box):
        if pending["box"] is None:
            pending["box"] = list(box)
            return
        b = pending["box"]
        b[0], b[1] = min(b[0], box[0]), min(b[1], box[1])
        b[2], b[3] = max(b[2], box[2]), max(b[3], box[3])

    def flush(force=False):
        if pending["box"] is None:
            return
        now = time.perf_counter()
        if not force and now - pending["last"] < interval:
            stats["coalesced"] += 1
            return
        if args.gap_ms:
            time.sleep(args.gap_ms / 1e3)
        send_rect(disp, img, tuple(pending["box"]), args.codec, stats)
        pending["box"] = None
        pending["last"] = time.perf_counter()

    try:
        with pud_usb.open_device() as disp:
            print("device caps=%s  mode=%s  codec=%s"
                  % (disp.caps, args.mode, args.codec))
            if disp.decoder_type is not None:
                want = {"jpeg": 1, "lz4": 2, "qoi": 3, "rle": 4}[args.codec]
                if disp.decoder_type != want:
                    sys.exit("the device is decoder_type=%s, not %d (%s); "
                             "rebuild it or pass a matching --codec"
                             % (disp.decoder_type, want, args.codec))

            img = make_canvas(args.mode)
            draw = ImageDraw.Draw(img)

            if args.mode == "targets":
                for i, (x, y) in enumerate(TARGETS):
                    target_mark(draw, x, y, "T%d" % (i + 1))
                print("touch T1..T5 in order, %s" % ", ".join(
                    "T%d=(%d,%d)" % (i + 1, x, y)
                    for i, (x, y) in enumerate(TARGETS)))
            elif args.mode == "trace":
                print("drag your finger on the panel; the trail is drawn "
                      "where the device reports it")
            else:
                print("touch anywhere; the label under the mark is the "
                      "reported coordinate")

            t0 = time.perf_counter()
            send_rect(disp, img, (0, 0, W - 1, H - 1), args.codec, stats)
            pending["last"] = time.perf_counter()
            print("canvas pushed in %.1f ms (%d bytes)"
                  % (stats["us"] / 1e3, stats["bytes"]))

            # targets state machine: wait for a press, take the first stable
            # report, wait for the release, then move to the next target
            index = 0
            held = None
            results = []
            deadline = None if args.seconds is None else t0 + args.seconds

            while deadline is None or time.perf_counter() < deadline:
                flush()
                try:
                    rep = disp.read_touch(timeout=int(args.timeout))
                except Exception as exc:
                    if type(exc).__name__ != "USBTimeoutError":
                        raise
                    misses += 1
                    continue

                if not rep["version"]:
                    print("! report version is 0: this firmware does not "
                          "implement touch (EP4 is a stub)")
                    return 1

                reports += 1
                x, y = rep["x"], rep["y"]
                if args.verbose:
                    print("%s state=%-7s x=%3d y=%3d seq=%3d"
                          % (time.strftime("%H:%M:%S"),
                             "PRESS" if rep["pressed"] else "release", x, y,
                             rep["seq"]))
                last = (x, y)

                if args.mode == "targets":
                    if rep["pressed"]:
                        if held is None:
                            held = (x, y)
                            tx, ty = TARGETS[index]
                            results.append((index, tx, ty, x, y))
                            colour = CURSOR[index % len(CURSOR)]
                            crosshair(draw, x, y, colour, size=args.width * 2)
                            draw.text((x + 12, y - 6), "T%d" % (index + 1),
                                      fill=colour, font=load_font(10))
                            mark_dirty((min(tx, x) - 16, min(ty, y) - 16,
                                        max(tx, x) + 24, max(ty, y) + 16))
                            flush()
                    elif held is not None:
                        held = None
                        index += 1
                        if index >= len(TARGETS):
                            break
                else:
                    if rep["pressed"]:
                        colour = CURSOR[0] if args.mode == "trace" \
                            else (255, 255, 255)
                        w = args.width
                        draw.rectangle([x - w, y - w, x + w, y + w],
                                       fill=colour)
                        if args.mode == "grid":
                            label = "%d,%d" % (x, y)
                            draw.text((x + 12, y - 6), label,
                                      fill=(200, 200, 0), font=load_font(10))
                        mark_dirty((x - w - 2, y - w - 2,
                                    x + w + 24, y + w + 12))
                        flush()

    except pud_usb.PudError as exc:
        sys.exit(str(exc))
    except KeyboardInterrupt:
        pass

    print()
    print("reports    : %d (%d idle timeouts)" % (reports, misses))
    print("last point : %s" % (last,))
    if stats["rects"]:
        print("panel      : %d rects, %d bytes, %.1f ms of host time "
              "(%.0f us/rect, %d updates deferred)"
              % (stats["rects"], stats["bytes"], stats["us"] / 1e3,
                 stats["us"] / stats["rects"], stats["coalesced"]))

    if results:
        print()
        print("%-4s %-14s %-14s %-10s" % ("", "target", "reported", "error"))
        dxs, dys = [], []
        for i, tx, ty, x, y in results:
            print("T%-3d %-14s %-14s dx=%+d dy=%+d"
                  % (i + 1, "(%d,%d)" % (tx, ty), "(%d,%d)" % (x, y),
                     x - tx, y - ty))
            dxs.append(x - tx)
            dys.append(y - ty)
        mean_dx = sum(dxs) / len(dxs)
        mean_dy = sum(dys) / len(dys)
        print()
        print("mean error : dx=%+.1f dy=%+.1f  (this is what x_offs/y_offs "
              "would cancel)" % (mean_dx, mean_dy))
        if len(results) > 1:
            xs = [r[3] for r in results]
            ys = [r[4] for r in results]
            print("span       : x %d y %d  (a rotated or swapped axis shows up "
                  "as a span that does not match the panel)"
                  % (max(xs) - min(xs), max(ys) - min(ys)))
            if max(xs) - min(xs) < 200 and max(ys) - min(ys) < 200:
                print("             both spans are small: not all targets were "
                      "hit, or the axes are swapped")
        print("note       : a capacitive panel bonded to the glass has a real "
              "edge offset, so a few pixels of mean error is normal")

    return 0


if __name__ == "__main__":
    sys.exit(main())
