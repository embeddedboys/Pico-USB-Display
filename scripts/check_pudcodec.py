#!/usr/bin/env python3
"""Check tools/pudcodec against the python encoders and the firmware assets.

There are two host paths to the same codec stream -- the C tool and
`pud_usb.py` -- and on a lossless source they must agree byte for byte.  This
script drives the built tool and compares; no device is involved.

    cd build:   cmake -S tools -B tools/build && cmake --build tools/build
    run:        python3 scripts/check_pudcodec.py

Checks:
  1. img2s (png source)  ==  the python encoder, byte for byte
  2. video2s --raw       ==  the python encoder, container intact
  3. s2img round trip    ==  the original pixels, exactly
  4. headers: codec tag, dimensions, auto detection, frame table
  5. jpeg: encode, then decode back through the tool
  6. the boot logo arrays embedded in include/bootlogo.h
"""

import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TOOL = REPO / "tools" / "build" / "pudcodec"
sys.path.insert(0, str(REPO / "scripts"))

W, H = 480, 320
fails = []


def run(*args, expect=0):
    out = subprocess.run([str(TOOL)] + [str(a) for a in args],
                         capture_output=True, text=True)
    if out.returncode != expect:
        fails.append(" ".join(str(a) for a in args))
        print("  !! exit %d: %s" % (out.returncode, out.stderr.strip()[-300:]))
    return out


def check(label, ok, detail=""):
    print("  %-52s %s %s" % (label, "OK " if ok else "FAIL", detail))
    if not ok:
        fails.append(label)


def pixels_of(path):
    """The RGB565 bytes the python path builds from this image."""
    return P.rgb888_to_rgb565(P.load_image(str(path), W, H, fit=False), W, H)


def repack(png):
    return P.rgb888_to_rgb565(P.load_image(str(png), W, H, fit=False), W, H)


def container_ok(blob, frames):
    """[u32 count][u32 offsets[count+1]][data...] with sane offsets."""
    if len(blob) < 8:
        return False, "too short"
    count = struct.unpack_from("<I", blob, 0)[0]
    if count != frames:
        return False, "count %d != %d" % (count, frames)
    offsets = struct.unpack_from("<%dI" % (count + 1), blob, 4)
    if offsets[0] != 4 + 4 * (count + 1) or offsets[-1] != len(blob):
        return False, "offsets %s in %d bytes" % (offsets, len(blob))
    if list(offsets) != sorted(offsets):
        return False, "offsets not monotonic"
    return True, "count=%d, %d bytes" % (count, len(blob))


def bootlogo_entry(name):
    """Bytes of one DECODER_TYPE branch of include/bootlogo.h."""
    text = (REPO / "include" / "bootlogo.h").read_text()
    markers = {"tjpgd": "#if DECODER_TYPE == 0", "lz4": "#elif DECODER_TYPE == 2",
               "qoi": "#elif DECODER_TYPE == 3", "rle": "#elif DECODER_TYPE == 4"}
    start = text.index(markers[name])
    ends = [i for i in (text.find("#elif DECODER_TYPE", start + 1),
                        text.find("#else", start + 1)) if i > 0]
    body = text[start:min(ends)]
    # the size comment ("480x320") contains a literal 0x32
    vals = re.findall(r"(?<![0-9a-zA-Z])0x([0-9a-fA-F]{2})", body)
    return bytes(int(v, 16) for v in vals)


def main():
    if not TOOL.exists():
        sys.exit("build it first: cmake -S tools -B tools/build && "
                 "cmake --build tools/build")

    global P
    try:
        import numpy as np
        from PIL import Image
        import pud_usb as P
    except ImportError as exc:
        sys.exit("this check needs numpy and Pillow: %s" % exc)

    tmp = Path(tempfile.mkdtemp(prefix="pudcodec-"))

    photo = tmp / "photo.png"
    Image.open(REPO / "assets" / "bootlogo.jpg").save(photo)
    noise = tmp / "noise.png"
    Image.fromarray(np.random.default_rng(7).integers(
        0, 256, (H, W, 3), dtype=np.uint8)).save(noise)
    grad = tmp / "grad.png"
    xs = np.tile((np.arange(W, dtype=np.uint16) * 255 // W)[None, :], (H, 1))
    ys = np.tile((np.arange(H, dtype=np.uint16) * 255 // H)[:, None], (1, W))
    Image.fromarray(np.stack([xs, ys, (xs + ys) // 2], -1)
                    .astype(np.uint8)).save(grad)

    print("1. img2s == the python encoder, byte for byte")
    for codec, encoder in (("qoi", P.qoi_encode), ("rle", P.rle_encode)):
        for image in (photo, noise, grad):
            out = tmp / ("%s.%s.bin" % (image.stem, codec))
            run("--codec", codec, "img2s", image, "-w", W, "-h", H,
                "-t", "bin", "-o", out)
            want = encoder(pixels_of(image))
            got = out.read_bytes() if out.exists() else b""
            first = next((i for i, (a, b) in enumerate(zip(got, want))
                          if a != b), min(len(got), len(want)))
            check("%s %-9s %6d bytes" % (codec, image.name, len(got)),
                  got == want,
                  "identical" if got == want else "differs at %d" % first)

    print("2. video2s --raw: container and frame data")
    raw = tmp / "frames.raw"
    raw.write_bytes(pixels_of(photo))
    for codec, encoder in (("qoi", P.qoi_encode), ("rle", P.rle_encode),
                           ("lz4", None)):
        out = tmp / ("raw.%s.bin" % codec)
        run("--codec", codec, "video2s", "--raw", raw, "-w", W, "-h", H,
            "-t", "bin", "-o", out)
        blob = out.read_bytes() if out.exists() else b""
        ok, detail = container_ok(blob, 1)
        check("%s container" % codec, ok, detail)
        if encoder is not None and ok:
            frame0 = blob[struct.unpack_from("<I", blob, 4)[0]:]
            check("%s frame 0 == the python encoder" % codec,
                  frame0 == encoder(pixels_of(photo)))

    print("3. s2img round trip is pixel exact (lossless codecs)")
    for codec in ("qoi", "rle", "lz4"):
        stream = tmp / ("rt.%s.bin" % codec)
        run("--codec", codec, "img2s", photo, "-w", W, "-h", H, "-t", "bin",
            "-o", stream)
        png = tmp / ("rt.%s.png" % codec)
        run("--codec", codec, "s2img", stream, "-w", W, "-h", H,
            "-t", "png", "-o", png)
        back = repack(png)
        same = int((np.frombuffer(back, np.uint16) ==
                    np.frombuffer(pixels_of(photo), np.uint16)).sum())
        check("%s round trip" % codec, back == pixels_of(photo),
              "%d/%d pixels identical" % (same, W * H))

    print("4. headers and auto detection")
    header = tmp / "logo.rle.h"
    run("--codec", "rle", "img2s", photo, "-n", "bootlogo", "-o", header)
    text = header.read_text()
    size = len(P.rle_encode(pixels_of(photo)))
    check("header: codec tag, width, height, size",
          '#define bootlogo_CODEC  "rle"' in text and
          "#define bootlogo_WIDTH  480" in text and
          "#define bootlogo_HEIGHT 320" in text and
          "#define bootlogo_SIZE   %du" % size in text)
    png = tmp / "auto.png"
    run("--codec", "auto", "s2img", header, "-t", "png", "-o", png)
    check("--codec auto reads the tag and decodes",
          repack(png) == pixels_of(photo))

    frames = []
    for i in range(3):
        f = tmp / ("f%d.png" % i)
        Image.fromarray(((np.asarray(Image.open(photo)).astype(int) + i * 7)
                         % 256).astype(np.uint8)).save(f)
        frames.append(f)
    clip = tmp / "clip.qoi.h"
    run("--codec", "qoi", "video2s", *frames, "-w", W, "-h", H, "-o", clip)
    text = clip.read_text()
    sizes = [len(P.qoi_encode(pixels_of(f))) for f in frames]
    check("video header: count, frame 0 size, total",
          "#define clip_FRAME_COUNT 3" in text and
          "#define clip_FRAME0_SIZE %du" % sizes[0] in text and
          "#define clip_TOTAL_SIZE  %du" % sum(sizes) in text)
    one = tmp / "frame0.png"
    run("--codec", "auto", "s2img", clip, "-o", one)
    check("video header: frame 0 decodes",
          repack(one) == pixels_of(frames[0]))
    bad = run("--codec", "bogus", "img2s", photo, expect=1)
    check("unknown codec is rejected", "unknown codec" in bad.stderr)

    print("5. jpeg")
    jpg = tmp / "logo.jpg"
    run("--codec", "jpeg", "img2s", photo, "-w", W, "-h", H, "-t", "bin",
        "-o", jpg)
    check("jpeg encode produced a jpeg", jpg.read_bytes()[:2] == b"\xff\xd8")
    back = tmp / "logo.jpg.png"
    run("--codec", "jpeg", "s2img", jpg, "-t", "png", "-o", back)
    got = np.frombuffer(P.load_image(str(back), W, H, fit=False), np.uint8)
    want = np.frombuffer(P.load_image(str(photo), W, H, fit=False), np.uint8)
    diff = np.abs(got.astype(int) - want.astype(int))
    check("jpeg round trip", diff.max() <= 24 and diff.mean() < 3.0,
          "max %d LSB, mean %.2f LSB" % (diff.max(), diff.mean()))

    print("6. the embedded boot logo arrays")
    for codec in ("qoi", "rle"):
        out = tmp / ("bootlogo.%s.bin" % codec)
        run("--codec", codec, "img2s", photo, "-w", W, "-h", H, "-t", "bin",
            "-o", out)
        got = out.read_bytes()
        want = bootlogo_entry(codec)
        check("%s: include/bootlogo.h %d B" % (codec, len(want)),
              got == want,
              "identical" if got == want else
              "tool says %d B, differs" % len(got))

    print()
    print("RESULT:", "all checks passed" if not fails
          else "%d FAILURES: %s" % (len(fails), fails[:6]))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
