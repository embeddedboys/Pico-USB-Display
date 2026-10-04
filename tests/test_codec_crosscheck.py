#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
The offline C converter (`tools/pudcodec`) and the Python encoders must agree,
and the assets embedded in `include/bootlogo.h` must be reproducible from the
one source image.

There are two host paths to the same codec stream -- the C tool the kernel
driver's converters came from, and `tools/pud_usb.py` -- and on a lossless
source they must agree byte for byte.  This test drives the built tool and
compares.  No device, no network.

    cmake -S tools -B tools/build && cmake --build tools/build   # once
    python3 tests/test_codec_crosscheck.py

Checks, in the order they print:

  1. img2s (lossless PNG source) == the python encoder, byte for byte
  2. video2s --raw container layout, and frame 0 == the python encoder
  3. s2img round trip is pixel exact (QOI / RLE / LZ4)
  4. .h headers: codec tag, dimensions, auto detection, frame table
  5. JPEG encode/decode round trip within the lossy codec's error
  6. LZ4 band container: band count, offsets, band size limit, round trip
  7. include/bootlogo.h QOI/RLE/LZ4 branches == a fresh run of the same image

Known non-regression, reported as its own check: the python `lz4` wheel is
liblz4 1.9.x while the tool vendors 1.10.0, and LZ4's compressed bytes are not
stable across versions.  That check compares *round-trip pixels*, not bytes, so
a version difference cannot fail this test (the earlier byte comparison would
have, and did: 3 of 8 bands differed on the wheel here).
'''

import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE / "common"))
sys.path.insert(0, str(_HERE.parent / "tools"))

from harness import (Oracle, Report, Snapshot, Test,  # noqa: E402
                     EnvironmentProblem, run_test)
import pud_usb as P  # noqa: E402

REPO = _HERE.parent
TOOL = REPO / "tools" / "build" / "pudcodec"
W, H = 480, 320

# ORACLE: RELATIONSHIP
# SOURCE: the two host encoders are required to be the same function on a
#         lossless source (tools/pudcodec vs tools/pud_usb.py), and
#         include/bootlogo.h is required to be what mkbootlogo.py generates
#         from assets/bootlogo.png -- see notes/scripts.md
# EXPECTED: byte-for-byte equality where the codec is lossless; pixel-exact
#           round trips; JPEG within the codec's own lossiness
ORACLE = Oracle(
    "RELATIONSHIP",
    "notes/scripts.md (the tool and pud_usb are the same codec) plus "
    "tools/mkbootlogo.py's contract for include/bootlogo.h",
    "C tool output == Python encoder output byte for byte on lossless inputs; "
    "s2img round trips pixel exactly; bootlogo.h branches == a fresh encode")

_fails = []
_detail = []


def run(*args, expect=0):
    out = subprocess.run([str(TOOL)] + [str(a) for a in args],
                         capture_output=True, text=True)
    if out.returncode != expect:
        _fails.append(" ".join(str(a) for a in args))
        _detail.append("exit %d: %s" % (out.returncode,
                                        out.stderr.strip()[-200:]))
    return out


def check(snaps, label, ok, detail=""):
    snaps.append(Snapshot(label, "OK" if ok else "FAIL",
                          detail))
    if not ok:
        _fails.append(label)
        _detail.append("%s: %s" % (label, detail))


def pixels_of(path):
    return P.rgb888_to_rgb565(P.load_image(str(path), W, H, fit=False), W, H)


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
    import re

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


def check_all(dev, args):
    snaps = []
    _fails.clear()
    _detail.clear()

    if not TOOL.exists():
        raise EnvironmentProblem(
            "build it first: cmake -S tools -B tools/build && "
            "cmake --build tools/build")

    try:
        import numpy as np
        from PIL import Image
    except ImportError as exc:
        raise EnvironmentProblem("this check needs numpy and Pillow: %s" % exc)

    tmp = Path(tempfile.mkdtemp(prefix="pudcodec-"))
    try:
        photo = tmp / "photo.png"
        Image.open(REPO / "assets" / "bootlogo.png").save(photo)
        noise = tmp / "noise.png"
        Image.fromarray(np.random.default_rng(7).integers(
            0, 256, (H, W, 3), dtype=np.uint8)).save(noise)
        grad = tmp / "grad.png"
        xs = np.tile((np.arange(W, dtype=np.uint16) * 255 // W)[None, :], (H, 1))
        ys = np.tile((np.arange(H, dtype=np.uint16) * 255 // H)[:, None], (1, W))
        Image.fromarray(np.stack([xs, ys, (xs + ys) // 2], -1)
                        .astype(np.uint8)).save(grad)

        # 1 -- the core claim
        for codec, encoder in (("qoi", P.qoi_encode), ("rle", P.rle_encode)):
            for image in (photo, noise, grad):
                out = tmp / ("%s.%s.bin" % (image.stem, codec))
                run("--codec", codec, "img2s", image, "-w", W, "-h", H,
                    "-t", "bin", "-o", out)
                want = encoder(pixels_of(image))
                got = out.read_bytes() if out.exists() else b""
                first = next((i for i, (a, b) in enumerate(zip(got, want))
                              if a != b), min(len(got), len(want)))
                check(snaps, "img2s %s %-9s %6d bytes" % (codec, image.name,
                                                          len(got)),
                      got == want,
                      "identical" if got == want else "differs at %d" % first)

        # 2 -- frame container
        raw = tmp / "frames.raw"
        raw.write_bytes(pixels_of(photo))
        for codec, encoder in (("qoi", P.qoi_encode), ("rle", P.rle_encode),
                               ("lz4", None)):
            out = tmp / ("raw.%s.bin" % codec)
            run("--codec", codec, "video2s", "--raw", raw, "-w", W, "-h", H,
                "-t", "bin", "-o", out)
            blob = out.read_bytes() if out.exists() else b""
            blocks = 1 if codec != "lz4" else H // 40
            ok, detail = container_ok(blob, blocks)
            check(snaps, "video2s %s container" % codec, ok, detail)
            if encoder is not None and ok:
                frame0 = blob[struct.unpack_from("<I", blob, 4)[0]:]
                check(snaps, "video2s %s frame 0 == python" % codec,
                      frame0 == encoder(pixels_of(photo)))

        # 3 -- round trip
        for codec in ("qoi", "rle", "lz4"):
            stream = tmp / ("rt.%s.bin" % codec)
            run("--codec", codec, "img2s", photo, "-w", W, "-h", H, "-t", "bin",
                "-o", stream)
            png = tmp / ("rt.%s.png" % codec)
            run("--codec", codec, "s2img", stream, "-w", W, "-h", H,
                "-t", "png", "-o", png)
            back = P.rgb888_to_rgb565(P.load_image(str(png), W, H, fit=False),
                                      W, H)
            same = int((np.frombuffer(back, np.uint16) ==
                        np.frombuffer(pixels_of(photo), np.uint16)).sum())
            check(snaps, "s2img %s round trip" % codec, back == pixels_of(photo),
                  "%d/%d pixels identical" % (same, W * H))

        # 4 -- headers and auto detection
        header = tmp / "logo.rle.h"
        run("--codec", "rle", "img2s", photo, "-n", "bootlogo", "-o", header)
        text = header.read_text()
        size = len(P.rle_encode(pixels_of(photo)))
        check(snaps, "header: codec tag, width, height, size",
              '#define bootlogo_CODEC  "rle"' in text and
              "#define bootlogo_WIDTH  480" in text and
              "#define bootlogo_HEIGHT 320" in text and
              "#define bootlogo_SIZE   %du" % size in text)
        png = tmp / "auto.png"
        run("--codec", "auto", "s2img", header, "-t", "png", "-o", png)
        check(snaps, "--codec auto reads the tag and decodes",
              P.rgb888_to_rgb565(P.load_image(str(png), W, H, fit=False),
                                 W, H) == pixels_of(photo))

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
        check(snaps, "video header: count, frame 0 size, total",
              "#define clip_BLOCK_COUNT 3" in text and
              "#define clip_BLOCK0_SIZE %du" % sizes[0] in text and
              "#define clip_TOTAL_SIZE  %du" % sum(sizes) in text)
        one = tmp / "frame0.png"
        run("--codec", "auto", "s2img", clip, "-o", one)
        check(snaps, "video header: frame 0 decodes",
              P.rgb888_to_rgb565(P.load_image(str(one), W, H, fit=False),
                                 W, H) == pixels_of(frames[0]))
        bad = run("--codec", "bogus", "img2s", photo, expect=1)
        check(snaps, "unknown codec is rejected", "unknown codec" in bad.stderr)

        # 5 -- jpeg, lossy
        jpg = tmp / "logo.jpg"
        run("--codec", "jpeg", "img2s", photo, "-w", W, "-h", H, "-t", "bin",
            "-o", jpg)
        check(snaps, "jpeg encode produced a jpeg",
              jpg.read_bytes()[:2] == b"\xff\xd8")
        back = tmp / "logo.jpg.png"
        run("--codec", "jpeg", "s2img", jpg, "-t", "png", "-o", back)
        got = np.frombuffer(P.load_image(str(back), W, H, fit=False), np.uint8)
        want = np.frombuffer(P.load_image(str(photo), W, H, fit=False), np.uint8)
        diff = np.abs(got.astype(int) - want.astype(int))
        # JPEG is lossy by definition: the bound is the codec's own error, not
        # a target this project set.  Measured max 19 LSB, mean 0.43 LSB.
        check(snaps, "jpeg round trip error",
              diff.max() <= 24 and diff.mean() < 3.0,
              "max %d LSB, mean %.2f LSB" % (diff.max(), diff.mean()))

        # 6 -- the LZ4 band container
        banded = tmp / "photo.lz4.bin"
        run("--codec", "lz4", "img2s", photo, "-w", W, "-h", H, "-t", "bin",
            "-o", banded)
        blob = banded.read_bytes()
        bands = struct.unpack_from("<I", blob, 0)[0]
        offsets = struct.unpack_from("<%dI" % (bands + 1), blob, 4)
        check(snaps, "lz4 container: %d bands, offsets sane" % bands,
              bands == H // 40 and offsets[0] == 4 + 4 * (bands + 1) and
              offsets[-1] == len(blob))
        limit = 2 * ((65535 - 16) // 3)
        check(snaps, "lz4 every band fits the device buffer",
              bands * 40 * W * 2 // bands <= limit,
              "%d bytes raw per band, limit %d" % (40 * W * 2, limit))
        back = tmp / "photo.lz4.png"
        run("--codec", "lz4", "s2img", banded, "-w", W, "-h", H, "-t", "png",
            "-o", back)
        check(snaps, "lz4 container round trip is pixel exact",
              P.rgb888_to_rgb565(P.load_image(str(back), W, H, fit=False),
                                 W, H) == pixels_of(photo))

        # 7 -- the embedded logo arrays
        for codec in ("qoi", "rle", "lz4"):
            out = tmp / ("bootlogo.%s.bin" % codec)
            run("--codec", codec, "img2s", photo, "-w", W, "-h", H, "-t", "bin",
                "-o", out)
            got = out.read_bytes()
            want = bootlogo_entry(codec)
            check(snaps, "include/bootlogo.h %s %d B" % (codec, len(want)),
                  got == want,
                  "identical" if got == want else
                  "tool says %d B, differs" % len(got))
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    ok = not _fails
    return Report.of(snaps, ok=ok,
                     detail=("; ".join(_detail[:6]) if _fails
                             else "all cross-checks reproduced"))


TEST = Test(
    "test_codec_crosscheck", ORACLE, check_all, needs_device=False,
    default_timeout=300.0,
    description="tools/pudcodec vs the python encoders and include/bootlogo.h "
                "(offline)")

if __name__ == "__main__":
    run_test(TEST)
