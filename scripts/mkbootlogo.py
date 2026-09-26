#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
Regenerate include/bootlogo.h from assets/bootlogo.png.

One branch per decoder family, each produced by tools/pudcodec from the same
asset -- jpeg (DECODER_TYPE 0 and 1), lz4 (2), qoi (3), rle (4).  The asset
has to be lossless: scripts/check_pudcodec.py rebuilds these streams from
assets/bootlogo.png through Pillow and expects them byte for byte, and with a
JPEG source the two decoders disagree (IDCT rounding).

    cmake -S tools -B tools/build && cmake --build tools/build
    python3 scripts/mkbootlogo.py            # rewrite include/bootlogo.h
    python3 scripts/mkbootlogo.py --check    # compare only, write nothing

bootlogo.h is generated, so this only rewrites the byte rows between each
array's braces and leaves the markers, comments and declarations where they
were -- and it parses its own result back and compares it against the tool's
output before anything is written.  (A batch edit of that file once emptied it,
which is what AGENTS.md rule 8 is about.)  The LZ4 branch is a
[count][offsets][blocks] container whose count has to divide the panel height;
the tool picks the row count, and --check reports both.
'''

import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HEADER = REPO / "include" / "bootlogo.h"
ASSET = REPO / "assets" / "bootlogo.png"
TOOL = REPO / "tools" / "build" / "pudcodec"

# the marker line that opens each branch, and how its rows are laid out
BRANCHES = [
    ("jpeg", "#if DECODER_TYPE == 0 || DECODER_TYPE == 1", 16, "  "),
    ("lz4", "#elif DECODER_TYPE == 2", 16, "\t"),
    ("qoi", "#elif DECODER_TYPE == 3", 12, "  "),
    ("rle", "#elif DECODER_TYPE == 4", 12, "  "),
]
W, H = 480, 320


def encode(codec):
    """Run the tool for one codec and return its bytes."""
    out = Path("/tmp/mkbootlogo.%s.bin" % codec)
    rc = subprocess.run([str(TOOL), "--codec", codec, "img2s", str(ASSET),
                         "-w", str(W), "-h", str(H), "-t", "bin", "-o", str(out)],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        sys.exit("pudcodec --codec %s failed:\n%s" % (codec, rc.stderr.strip()))
    return out.read_bytes()


def find_array(lines, marker):
    """(first byte row, closing brace) of the array after the marker line."""
    start = next((i for i, l in enumerate(lines) if l.strip() == marker), None)
    if start is None:
        sys.exit("marker missing from %s: %s" % (HEADER, marker))
    decl = next(i for i in range(start, len(lines))
                if lines[i].startswith("const unsigned char bootlogo[]"))
    first = next(i for i in range(decl + 1, len(lines))
                 if lines[i].strip().startswith("0x"))
    close = next(i for i in range(first, len(lines)) if lines[i].strip() == "};")
    return first, close


def bytes_of(lines, first, close):
    text = "".join(lines[first:close])
    return bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})", text))


def rows_of(data, per_line, indent):
    rows = []
    for i in range(0, len(data), per_line):
        rows.append(indent + " ".join("0x%02x," % b
                                      for b in data[i:i + per_line]).rstrip())
    rows[-1] = rows[-1].rstrip(",")      # the file has no comma on the last row
    return rows


def lz4_summary(data):
    count = struct.unpack_from("<I", data, 0)[0]
    return "%d blocks of %d rows" % (count, H // count if count else 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--check", action="store_true",
                    help="compare only; do not write include/bootlogo.h")
    args = ap.parse_args()

    if not TOOL.exists():
        sys.exit("build the tool first: cmake -S tools -B tools/build && "
                 "cmake --build tools/build")

    lines = HEADER.read_text().splitlines(keepends=True)
    streams = {name: encode(name) for name, _, _, _ in BRANCHES}

    stale = []
    for (name, marker, per_line, indent) in BRANCHES:
        first, close = find_array(lines, marker)
        have = bytes_of(lines, first, close)
        want = streams[name]
        state = "identical" if have == want else "DIFFERS"
        print("  %-5s %7d bytes in the file, %7d from the tool   %s"
              % (name, len(have), len(want), state))
        if have != want:
            stale.append(name)
    print("  lz4   %s" % lz4_summary(streams["lz4"]))

    if args.check:
        if stale:
            print("\n%s out of date: %s" % (HEADER, ", ".join(stale)))
            return 1
        print("\n%s matches the tool for every branch" % HEADER)
        return 0

    out = list(lines)
    for (name, marker, per_line, indent) in reversed(BRANCHES):
        first, close = find_array(out, marker)
        decl = first - 1                     # the declaration line
        out[first:close] = [r + "\n" for r in
                            rows_of(streams[name], per_line, indent)]
        # the comment above each array quotes its size ("// array size is N",
        # "// N bytes, 480x320"); refresh any long number on those lines
        for i in range(max(0, decl - 8), decl):
            if out[i].startswith("//") and re.search(r"\d{4,}", out[i]):
                out[i] = re.sub(r"\d{4,}", str(len(streams[name])), out[i])

    text = "".join(out)
    check = text.splitlines(keepends=True)
    for (name, marker, per_line, indent) in BRANCHES:
        first, close = find_array(check, marker)
        if bytes_of(check, first, close) != streams[name]:
            sys.exit("refusing to write: the %s branch does not round trip" % name)

    HEADER.write_text(text)
    print("\nwrote %s: %d -> %d lines" % (HEADER, len(lines),
                                          len(text.splitlines())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
