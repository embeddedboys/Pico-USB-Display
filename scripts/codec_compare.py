#!/usr/bin/env python3
"""Compare the RGB565 codecs on the device.

Both streams are produced in Python (``pud_usb.qoi_encode`` / ``rle_encode``,
each checked byte-for-byte against its C library by the module self-test) and
banded exactly the way the driver bands a damage rectangle, so the only thing
that differs between two columns is what the *firmware* does with the stream:
decode it and push it to the panel.

The device must be flashed with the matching ``DECODER_TYPE`` -- run this once
per codec and compare the two runs:

    # with DECODER_TYPE=3 (QOI)
    ./scripts/codec_compare.py --frames 100 --codec qoi
    # then flash DECODER_TYPE=4 (RLE) and
    ./scripts/codec_compare.py --frames 100 --codec rle

Measurements use ``fps_bench.measure()`` (control request + bulk transfer, no
encoding inside the timing loop) so the numbers are comparable with the ones in
notes/scripts.md.
"""

import argparse
import statistics
import sys

import fps_bench as B
import pud_usb as P

CODECS = {
    "qoi": P.qoi_encode,
    "rle": P.rle_encode,
}


def build_frames(px, encode, w, h):
    """Band one window the way the driver does, then encode each band.

    A window small enough that the encoded band fits one transfer (<= band
    pixels) is sent as a single transfer, which is what makes the timing
    decode-bound instead of USB-bound.
    """
    frames = []
    for (xs, ys, xe, ye) in B.band_rects(0, 0, w - 1, h - 1):
        band = P.crop_rgb565(px, B.XRES, xs, ys, xe - xs + 1, ye - ys + 1)
        frames.append((xs, ys, xe, ye, encode(band)))
    return frames


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--frames", type=int, default=100,
                    help="frames per case (default 100)")
    ap.add_argument("--pattern", default="solid,gradient,photo,noise",
                    help="content to compare (default all four)")
    ap.add_argument("--image", default="assets/xfce.jpg",
                    help="image for the photo pattern")
    ap.add_argument("--codec", default="qoi,rle",
                    help="codecs to send (only the flashed one decodes)")
    ap.add_argument("--window", default="480x320", metavar="WxH",
                    help="window to send; small windows fit one transfer, "
                         "which measures the decoder instead of the bus")
    args = ap.parse_args()

    try:
        w, h = (int(v) for v in args.window.lower().split("x"))
    except ValueError:
        sys.exit("bad --window %r, expected WxH" % args.window)
    if not (0 < w <= B.XRES and 0 < h <= B.YRES):
        sys.exit("window %dx%d out of range" % (w, h))

    patterns = [p.strip() for p in args.pattern.split(",") if p.strip()]
    codecs = [(c.strip(), CODECS[c.strip()]) for c in args.codec.split(",")
              if c.strip()]

    with P.open_device() as disp:
        print("device %04x:%04x  caps=%s  frame_max=%d  band=%d px  window=%dx%d"
              % (P.VID, P.PID, disp.caps, disp.frame_max, disp.band_pixels, w, h))
        print()
        print("%-9s %-4s %9s %8s %10s %10s %9s" %
              ("内容", "编码", "字节/帧", "压缩率", "稳态", "min", "帧率"))
        print("-" * 68)
        for name in patterns:
            full = B.pattern_rgb565(name, 0, args.image)
            px = P.crop_rgb565(full, B.XRES, 0, 0, w, h)
            raw = w * h * 2
            for cname, encode in codecs:
                frames = build_frames(px, encode, w, h)
                payload = sum(len(f[4]) for f in frames)
                periods = B.measure(disp, [frames], args.frames)
                med = statistics.median(periods)
                print("%-9s %-4s %9d %7.1f%% %8.2f ms %8.2f ms %7.1f fps"
                      % (name, cname, payload, 100.0 * payload / raw,
                         med * 1000, min(periods) * 1000, 1.0 / med))
            print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
