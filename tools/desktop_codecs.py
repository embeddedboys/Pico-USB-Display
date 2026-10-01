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

    ./tools/desktop_codecs.py                      # sizes only
    ./tools/desktop_codecs.py --device --codec lz4 # + timing, LZ4 firmware
    ./tools/desktop_codecs.py --codec qoid         # + the cross-frame table

Requires Pillow; numpy and the lz4 package are optional.  The LZ4 sizes come
from tools/pudcodec (build it first), so the host side needs no lz4 binding.

`--codec qoid` adds a second host-side table for the experimental cross-frame
dictionary (DECODER_TYPE 6), which a per-rectangle table cannot express: it
needs the *previous* frame's band as a dictionary, and whether the device's
frame slots actually still hold that band.  That table prints payload bytes
only -- no times -- because the only rate available here is the LINK_BYTES_PER_S
model, which is exactly what the table above keeps being misread as.
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
#: Fallback transfer rate for the host-side table, in bytes/s.  This is a
#: *model*, not a measurement of the run you are looking at: the number comes
#: from one session in 2026-09, and the same board behind the same kind of root
#: port has since measured 0.935 MB/s -- 15% lower.  With --device the table is
#: calibrated with a real transfer instead (calibrate_link()) and the header says
#: which of the two it used.  A modelled time standing next to a measured one is
#: how a codec ends up looking slower than it is, which is a mistake this file
#: made: its QOI column was quoted as a device measurement for a whole session.
LINK_BYTES_PER_S = 1.1e6

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


def damage(img):
    """The same desktop one edit later: a clock tick and a retitled window.

    Two small edits in two places is what a partial refresh actually carries,
    and it is what makes one band a nearly-unchanged one (the panel bar) and
    another a changed one (the title bar).
    """
    out = img.copy()
    d = ImageDraw.Draw(out)
    d.rectangle([420, 4, 476, 20], fill=(38, 38, 44))
    d.text((424, 6), "13:46", font=_font("DejaVuSans.ttf", 11),
           fill=(225, 225, 230))
    d.rectangle([144, 180, 332, 202], fill=(74, 78, 88))
    d.text((148, 184), "Downloads", font=_font("DejaVuSans-Bold.ttf", 11),
           fill=(240, 240, 245))
    return out


def pixels(image):
    return P.rgb888_to_rgb565(P.load_image(str(image), W, H, fit=False), W, H)


def panel_image(image):
    """`image` at panel size as a PIL image, i.e. something `damage()` can edit."""
    return Image.frombytes("RGB", (W, H),
                           P.load_image(str(image), W, H, fit=False))


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


def host_table(images, tmp, rate=LINK_BYTES_PER_S, origin="constant"):
    """Payload sizes per codec for every region of each workload image."""
    print("payload bytes per rectangle; the time in brackets is %s at %.3f MB/s"
          % (origin, rate / 1e6))
    print("qoid is a KEYFRAME (16 B sub-header included): one frame per "
          "rectangle, so there is no history to preset")
    print("%-20s %7s %8s | %8s %8s %8s %8s | %s"
          % ("region", "pixels", "raw", "qoi", "rle", "lz4", "qoid",
             "smallest"))
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
                     "lz4": lz4_payload(raw, w, h, tmp),
                     "qoid": len(P.qoid_encode(raw))}
            sizes = {k: v for k, v in sizes.items() if v is not None}
            if not sizes:
                continue
            best = min(sizes, key=sizes.get)
            for k, v in sizes.items():
                total[k] = total.get(k, 0) + v
            print("%-20s %7d %8d | %8s %8s %8s %8s | %-3s %6d B (%.2f ms)"
                  % (name, w * h, len(raw),
                     *[sizes.get(k, "-") for k in ("qoi", "rle", "lz4", "qoid")],
                     best, sizes[best],
                     sizes[best] / rate * 1e3))
        print("   sum over the listed regions (they overlap; for ranking "
              "only): %s" % ", ".join("%s %d B" % (k, v)
                                      for k, v in sorted(total.items())))
        print()


#: DECODER_TYPE 6 payloads are deflated against the previous frame's band, so
#: whether a band can be a delta at all is decided by which band the device's
#: frame slot still holds (see pud_usb.Display.send_rgb565).  The pair table
#: below shows both that decision and what the codec would do if every band had
#: its own history.
class NullDevice:
    """A "device" that only records the transfers it would have been sent."""

    def __init__(self):
        self.writes = []

    def write(self, endpoint, data, timeout=None):
        self.writes.append(bytes(data))


def banded_payloads(writes):
    """(rect, payload) per captured EP1 transfer, payload size as declared."""
    out = []
    for blob in writes:
        xs, ys, xe, ye, size = P.EP1_HEADER.unpack_from(blob)
        out.append(((xs, ys, xe, ye),
                    blob[P.EP1_HEADER_SIZE:P.EP1_HEADER_SIZE + size]))
    return out


def qoid_pair_table(before, after):
    """The cross-frame dictionary (DECODER_TYPE 6) on two consecutive frames.

    Payload bytes only: the time column of the table above is the
    LINK_BYTES_PER_S model, and what this table is about is that the payload
    shrinks -- quoting a modelled time for it would repeat the mistake that
    column is already famous for.

    The second frame is sent through the real submission path
    (`Display.send_rgb565(codec="qoid")`) into a recorder, so the numbers are
    the bytes a host would put on the wire, 16 byte sub-header included, and
    keyframe-vs-delta is the decision the frame slots actually allow: a delta
    needs the slot this band lands in to still hold the *same rectangle*
    `DECODER_FRAME_SLOTS` submissions back, which after three warming frames
    happens when the region's band count is a multiple of that, and always when
    the region is the only thing being sent.  The last line is the all-delta
    case, i.e. what the codec is for but what this banding does not give.
    """
    px_a = P.rgb888_to_rgb565(before.tobytes(), W, H)
    px_b = P.rgb888_to_rgb565(after.tobytes(), W, H)
    print("cross-frame dictionary (DECODER_TYPE 6), two consecutive frames, "
          "banded as an RP2350 asks for")
    print("payload bytes only; the region was already sent %d times, because "
          "the slots need that long to hold a history"
          % P.DECODER_FRAME_SLOTS)
    print("%-20s %5s %9s %9s %9s %4s %5s %8s"
          % ("region", "bands", "qoi", "qoiz", "qoid", "key", "delta",
             "vs qoi"))
    print("-" * 82)
    total = dict.fromkeys(("bands", "keys", "deltas", "qoi", "qoiz", "qoid",
                           "key", "delta", "over"), 0)
    all_delta = 0
    for label, rect in REGIONS:
        x0, y0, x1, y1 = rect
        w, h = x1 - x0, y1 - y0
        raw_a, raw_b = crop(px_a, rect), crop(px_b, rect)

        dev = NullDevice()
        disp = P.Display(dev)
        for _ in range(P.DECODER_FRAME_SLOTS):
            disp.send_rgb565(raw_a, w, h, xs=x0, ys=y0, codec="qoid")
        sent = len(dev.writes)
        disp.send_rgb565(raw_b, w, h, xs=x0, ys=y0, codec="qoid")

        sizes = dict.fromkeys(("qoi", "qoiz", "qoid", "key", "delta"), 0)
        counts = dict.fromkeys(("key", "delta"), 0)
        over = 0
        for (xs, ys, xe, ye), payload in banded_payloads(dev.writes[sent:]):
            band = (xs, ys, xe + 1, ye + 1)
            qoi_a = P.qoi_encode(crop(px_a, band))
            qoi_b = P.qoi_encode(crop(px_b, band))
            flags = P.QOID_SUBHEADER.unpack_from(payload)[1]
            kind = "delta" if flags & P.QOID_FLAG_DELTA else "key"
            sizes["qoi"] += len(qoi_b)
            sizes["qoiz"] += len(P.qoiz_encode(crop(px_b, band)))
            sizes["qoid"] += len(payload)
            sizes[kind] += len(payload)
            counts[kind] += 1
            all_delta += len(P.qoid_pack(qoi_b, qoi_a))
            # the device holds history and output in one window, half each: a
            # band over that is refused as oversize, keyframe or not
            over += len(qoi_b) > P.PUD_DELTA_WIN // 2

        if not sizes["qoi"]:
            continue
        for k, v in sizes.items():
            total[k] += v
        total["bands"] += counts["key"] + counts["delta"]
        total["keys"] += counts["key"]
        total["deltas"] += counts["delta"]
        total["over"] += over
        print("%-20s %5d %9d %9d %9d %4d %5d %+7.1f%%"
              % (label + ("*" * min(over, 1)), counts["key"] + counts["delta"],
                 sizes["qoi"], sizes["qoiz"], sizes["qoid"], counts["key"],
                 counts["delta"], 100.0 * (sizes["qoid"] / sizes["qoi"] - 1.0)))
    print("-" * 82)
    print("%-20s %5d %9d %9d %9d %4d %5d %+7.1f%%"
          % ("sum (regions overlap)", total["bands"], total["qoi"],
             total["qoiz"], total["qoid"], total["keys"], total["deltas"],
             100.0 * (total["qoid"] / total["qoi"] - 1.0)))
    print("%d of the %d bands were deltas (%d B); the other %d had to be "
          "keyframes (%d B):" % (total["deltas"], total["bands"],
                                 total["delta"], total["keys"], total["key"]))
    print("  a band is a keyframe only when no band is resident yet, when the "
          "band it names")
    print("  has been evicted from every window, or when its QOI is over half a "
          "window.  The")
    print("  device searches its %d windows for whatever band the payload names, "
          "so the dictio-"
          % P.DECODER_FRAME_SLOTS)
    print("  nary does not have to be the same rectangle -- a vertically "
          "adjacent band is used")
    print("  when the same one is not resident, which is the only way a "
          "full-screen resend gets")
    print("  any delta at all.  The vs-qoi column of those rows is Z_FIXED "
          "deflate without a")
    print("  dictionary, i.e. qoiz less the dynamic Huffman.")
    print("if every band had its history in its slot: qoi %d B -> qoid %d B "
          "(%+.1f%%) -- what the codec is" % (total["qoi"], all_delta,
                                               100.0 * (all_delta / total["qoi"]
                                                        - 1.0)))
    print("  for, but not what this banding gives.")
    if total["over"]:
        print("* %d of the frame-2 bands have a QOI over the %d B half window "
              "(PUD_DELTA_WIN / 2): the device"
              % (total["over"], P.PUD_DELTA_WIN // 2))
        print("  refuses those as oversize -- qoid_oversize, no truncation -- "
              "so that region of the panel stays stale.")
    print()


# ---------------------------------------------------------------------------
# Device side: per-rectangle round trip
# ---------------------------------------------------------------------------

def calibrate_link(px, codec, frames=3):
    """Measure the link with the payload this run sends, and return bytes/s.

    One full-screen transfer, banded the way the device asks for, through the
    same submission path the measurement loop uses.  Without this the host table
    would state its times at a constant nobody re-measured; with it the two
    tables are on one ruler.
    """
    import fps_bench as B

    with P.open_device() as disp:
        bands = []
        for label, rect in REGIONS:
            if label != "full screen":
                continue
            x0, y0, x1, y1 = rect
            rows = max(1, disp.band_pixels // (x1 - x0))
            for y in range(y0, y1, rows):
                bh = min(rows, y1 - y)
                bands.append((x0, y, x1 - 1, y + bh - 1,
                              P.ENCODERS[codec](crop(px, (x0, y, x1, y + bh)))))
        nbytes = sum(len(b[4]) for b in bands)
        med = statistics.median(B.measure(disp, [bands], frames))
        print("link %.3f MB/s -- measured now, %s, %d bands, %d B, median of %d"
              % (nbytes / med / 1e6, codec, len(bands), nbytes, frames))
        return nbytes / med


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
        want = P.DECODER_TYPES[args.codec]
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
    ap.add_argument("--codec", default="qoi", choices=sorted(P.ENCODERS),
                    help="codec the device is built for; qoid also prints the "
                         "host-side cross-frame table (default qoi)")
    ap.add_argument("--image", default=None,
                    help="use this png as the workload (default: generate a "
                         "synthetic desktop)")
    ap.add_argument("--frames", type=int, default=40)
    ap.add_argument("--workdir", default=None,
                    help="where to write the generated workload (default: a "
                         "temporary directory)")
    ap.add_argument("--link-rate", type=float, default=None, metavar="MBPS",
                    help="rate for the host table's time column, in MB/s; "
                         "without --device the column is a model at the build-in "
                         "constant, and this is how to state it at the rate you "
                         "actually measured")
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

    rate, origin = LINK_BYTES_PER_S, "a model at a constant from an earlier session, NOT a measurement"
    if args.link_rate is not None:
        rate, origin = args.link_rate * 1e6, "--link-rate"
    elif args.device:
        rate, origin = calibrate_link(pixels(images[0][1]), args.codec), "measured in this run"
    host_table(images, tmp, rate, origin)
    if args.codec == "qoid":
        # the cross-frame codec needs a second frame, which a per-rectangle
        # payload table cannot express -- and no time column, see the docstring
        base = panel_image(images[0][1])
        qoid_pair_table(base, damage(base))
    if args.device:
        device_table_px(args, pixels(images[0][1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
