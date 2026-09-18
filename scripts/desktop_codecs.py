#!/usr/bin/env python3
"""Compare the codecs on the workload this project actually has: a desktop.

The panel mirrors a DRM desktop, so the content is a wallpaper with UI on top
and the traffic is mostly *partial* updates -- a text line, a list row, a
window body -- not full-screen photos.  That is a different workload from the
full-screen patterns in `fps_bench.py`, and it ranks the codecs differently:
QOI's decoder costs a few hundred ns per pixel, which is hidden behind the
transfer on a full screen but is most of the frame time on a small damage
rectangle, where LZ4 wins by more than its compression alone would explain.

Two halves, both optional:

  host side (always):  payload bytes per codec for every region
  device side (--device): per-region round trip for the codec the device is
                          built with (DECODER_TYPE); rebuild/flash to compare

    ./scripts/desktop_codecs.py                      # sizes only
    ./scripts/desktop_codecs.py --device --codec lz4 # + timing, LZ4 firmware

Requires Pillow; numpy and the lz4 package are optional.  The LZ4 sizes come
from tools/pudcodec (build it first), so the host side needs no lz4 binding.
"""

import argparse
import statistics
import struct
import subprocess
import sys
import time
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
import pud_usb as P  # noqa: E402

W, H = 480, 320
WALLPAPER = REPO / "assets" / "xfce.jpg"
TOOL = REPO / "tools" / "build" / "pudcodec"
FONT_DIR = Path("/usr/share/fonts/truetype/dejavu")
LINK_BYTES_PER_S = 1.1e6        # measured EP1 throughput

#: Rectangles a desktop plausibly damages, in panel coordinates.  Small ones
#: dominate: that is what window redraws and text edits produce.
REGIONS = [
    ("panel bar", (0, 0, W, 24)),
    ("terminal text line", (20, 96, 292, 110)),
    ("list row", (150, 230, 390, 246)),
    ("window title bar", (140, 180, 400, 202)),
    ("flat window body", (140, 202, 400, 262)),
    ("terminal body", (12, 60, 300, 200)),
    ("wallpaper strip", (0, 24, W, 84)),
    ("full screen", (0, 0, W, H)),
]

DECODER_TYPE = {"jpeg": 1, "lz4": 2, "qoi": 3, "rle": 4}


# ---------------------------------------------------------------------------
# The workload
# ---------------------------------------------------------------------------

def _font(name, size):
    return ImageFont.truetype(str(FONT_DIR / name), size)


def _gradient(w, h, top, bottom):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(h - 1, 1)
        row = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = row
    return img


def _dither(img, step=4):
    """Ordered dithering, i.e. the high-entropy wallpaper a codec cannot treat
    as a smooth gradient."""
    out = img.copy()
    px = out.load()
    w, h = out.size
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            n = ((x % step) - step // 2) + ((y % step) - step // 2)
            px[x, y] = (max(0, min(255, r + n)), max(0, min(255, g + n)),
                        max(0, min(255, b + n)))
    return out


def desktop_frame(dither_wallpaper=False):
    """A desktop: the repo's own XFCE wallpaper with a panel and two windows."""
    if dither_wallpaper:
        img = _dither(_gradient(W, H, (24, 32, 58), (46, 96, 110)))
    else:
        img = Image.open(WALLPAPER).convert("RGB").resize((W, H))
    d = ImageDraw.Draw(img)
    small, mono = _font("DejaVuSans.ttf", 11), _font("DejaVuSansMono.ttf", 11)
    bold = _font("DejaVuSans-Bold.ttf", 11)

    d.rectangle([0, 0, W, 23], fill=(38, 38, 44))
    for x, label in ((8, "Applications"), (86, "Places")):
        d.text((x, 6), label, font=small, fill=(225, 225, 230))
    d.text((132, 6), "Terminal", font=bold, fill=(255, 255, 255))
    d.text((W - 56, 6), "13:45", font=small, fill=(225, 225, 230))
    for i, col in enumerate([(200, 80, 80), (220, 190, 90), (120, 200, 120)]):
        d.rectangle([W - 76 + i * 7, 8, W - 72 + i * 7, 16], fill=col)

    d.rectangle([12, 60, 300, 200], fill=(28, 28, 32))
    d.rectangle([12, 60, 300, 76], fill=(58, 58, 66))
    d.text((20, 63), "user@desk: ~/src", font=small, fill=(235, 235, 235))
    lines = ["$ make -j8", "  CC   src/decoders/decoder.o",
             "  CC   src/cherryusb/usb.o", "  LD   build/pico-usb-display.elf",
             "   text    data     bss     dec", "  90908   66812  152132  309852",
             "$ git log --oneline -3", "2118fb8 decoder: redesign the LZ4 decode",
             "f295521 decoders: compile with function sections",
             "52ac0d3 scripts: band LZ4 and pick the codec",
             "$ ./tools/build/pudcodec --codec lz4 img2s a.png",
             "  bands: 8 block(s), rows of 40"]
    for i, line in enumerate(lines):
        d.text((20, 84 + i * 9), line, font=mono,
               fill=(210, 240, 210) if line.startswith("$") else (200, 200, 205))

    d.rectangle([140, 180, 400, 262], fill=(52, 54, 60))
    d.rectangle([140, 180, 400, 202], fill=(74, 78, 88))
    d.text((148, 184), "Documents", font=bold, fill=(240, 240, 245))
    d.text((378, 184), "x", font=small, fill=(240, 240, 245))
    for i, name in enumerate(["notes.pdf", "budget.ods", "photo.jpg",
                              "readme.txt"]):
        y = 206 + i * 14
        d.rectangle([146, y, 158, y + 10], fill=(120, 160, 220))
        d.text((164, y), name, font=small, fill=(230, 230, 235))
    d.rectangle([396, 204, 398, 258], fill=(96, 100, 110))

    d.polygon([(200, 140), (200, 156), (205, 151), (209, 156)],
              fill=(255, 255, 255), outline=(20, 20, 20))
    return img


def pixels(image):
    return P.rgb888_to_rgb565(P.load_image(str(image), W, H, fit=False), W, H)


def crop(px, rect):
    x0, y0, x1, y1 = rect
    return b"".join(px[(y * W + x0) * 2:(y * W + x1) * 2]
                    for y in range(y0, y1))


# ---------------------------------------------------------------------------
# Host side: payload sizes
# ---------------------------------------------------------------------------

def lz4_payload(raw, w, h, tmp):
    """Bytes the host would send for this rectangle, LZ4-banded.

    Uses tools/pudcodec because this machine has no python lz4 binding; that
    is the same liblz4 the firmware links.
    """
    if not TOOL.exists():
        return None
    src, out = tmp / "r.raw", tmp / "r.lz4.bin"
    src.write_bytes(raw)
    res = subprocess.run([str(TOOL), "--codec", "lz4", "video2s", "--raw",
                          str(src), "-w", str(w), "-h", str(h), "-t", "bin",
                          "-o", str(out)], capture_output=True)
    if res.returncode != 0:
        return None
    blob = out.read_bytes()
    count, = struct.unpack_from("<I", blob, 0)
    off = struct.unpack_from("<%dI" % (count + 1), blob, 4)
    return sum(off[i + 1] - off[i] for i in range(count)) + 12 * count


def host_table(images, tmp):
    """Payload sizes per codec for every region of each workload image."""
    print("payload bytes per rectangle, and the transfer time at %.1f MB/s"
          % (LINK_BYTES_PER_S / 1e6))
    print("%-20s %7s %8s | %8s %8s %8s | %s"
          % ("region", "pixels", "raw", "qoi", "rle", "lz4", "smallest"))
    print("-" * 82)
    for label, image in images:
        px = pixels(image)
        print("== %s" % label)
        total = {}
        for name, rect in REGIONS:
            raw = crop(px, rect)
            w, h = rect[2] - rect[0], rect[3] - rect[1]
            sizes = {"qoi": len(P.qoi_encode(raw)),
                     "rle": len(P.rle_encode(raw)),
                     "lz4": lz4_payload(raw, w, h, tmp)}
            sizes = {k: v for k, v in sizes.items() if v is not None}
            if not sizes:
                continue
            best = min(sizes, key=sizes.get)
            for k, v in sizes.items():
                total[k] = total.get(k, 0) + v
            print("%-20s %7d %8d | %8s %8s %8s | %-3s %6d B (%.2f ms)"
                  % (name, w * h, len(raw),
                     *[sizes.get(k, "-") for k in ("qoi", "rle", "lz4")],
                     best, sizes[best],
                     sizes[best] / LINK_BYTES_PER_S * 1e3))
        print("   sum over the listed regions (they overlap; for ranking "
              "only): %s" % ", ".join("%s %d B" % (k, v)
                                      for k, v in sorted(total.items())))
        print()


# ---------------------------------------------------------------------------
# Device side: per-rectangle round trip
# ---------------------------------------------------------------------------

def device_table_px(args, px):
    """Round trip per rectangle, with the host encoding kept out of the loop.

    The encoding is done up front and reported separately: the python QOI and
    RLE encoders are hand written and cost tens of milliseconds per frame,
    while LZ4 goes through liblz4, so timing them together would compare python
    interpreters instead of codecs (measured, see notes/decoders.md).  Same
    convention as fps_bench.measure(), which this reuses.
    """
    import fps_bench as B

    with P.open_device() as disp:
        want = DECODER_TYPE[args.codec]
        if disp.decoder_type is not None and disp.decoder_type != want:
            sys.exit("device reports decoder_type=%s, need %d (%s); rebuild it"
                     % (disp.decoder_type, want, args.codec))
        print("device %s  codec=%s  %d frames per region"
              % (disp.caps, args.codec, args.frames))
        print("%-20s %7s %9s %10s %10s %9s"
              % ("region", "pixels", "bytes", "median", "bandwidth", "encode"))
        print("-" * 72)
        for label, rect in REGIONS:
            x0, y0, x1, y1 = rect
            width, height = x1 - x0, y1 - y0
            rows = max(1, disp.band_pixels // width)

            t0 = time.perf_counter()
            bands = []
            for y in range(0, height, rows):
                bh = min(rows, height - y)
                raw = crop(px, (x0, y0 + y, x1, y0 + y + bh))
                bands.append((x0, y0 + y, x1 - 1, y0 + y + bh - 1,
                              P.ENCODERS[args.codec](raw)))
            encode_ms = (time.perf_counter() - t0) * 1e3

            nbytes = sum(len(b[4]) for b in bands)
            periods = []
            for _ in range(args.frames):
                if args.gap_ms:
                    time.sleep(args.gap_ms / 1e3)
                periods += B.measure(disp, [bands], 1)
            med = statistics.median(periods)
            print("%-20s %7d %9d %8.2f ms %8.2f MB/s %7.1f ms"
                  % (label, width * height, nbytes, med * 1e3,
                     nbytes / med / 1e6 if med else 0.0, encode_ms))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", action="store_true",
                    help="also measure on the device (it must be built for "
                         "--codec)")
    ap.add_argument("--codec", default="qoi", choices=sorted(P.ENCODERS))
    ap.add_argument("--image", default=None,
                    help="use this png as the workload (default: generate a "
                         "synthetic desktop)")
    ap.add_argument("--frames", type=int, default=40)
    ap.add_argument("--workdir", default=None,
                    help="where to write the generated workload (default: a "
                         "temporary directory)")
    ap.add_argument("--gap-ms", type=float, default=0.0,
                    help="pause between transfers (default 0: a tight loop of "
                         "small ones was measured not to break anything)")
    args = ap.parse_args()

    import tempfile

    tmp = Path(args.workdir or tempfile.mkdtemp(prefix="deskcodecs-"))
    tmp.mkdir(parents=True, exist_ok=True)

    if args.image is not None:
        # a real screenshot, e.g. one taken on the board
        image = Path(args.image)
        if not image.exists():
            sys.exit("no such workload image: %s" % image)
        images = [(image.stem, image)]
    else:
        for name, flag in (("desktop.png", False),
                           ("desktop_dither.png", True)):
            desktop_frame(flag).save(tmp / name)
        images = [("photo wallpaper", tmp / "desktop.png"),
                  ("dithered wallpaper", tmp / "desktop_dither.png")]

    host_table(images, tmp)
    if args.device:
        device_table_px(args, pixels(images[0][1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
