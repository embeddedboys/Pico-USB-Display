#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
The Python encoders must reproduce the C libraries byte for byte.

This is the offline anchor for every host-side payload the tests and tools
send: `tools/pud_usb.py` claims its QOI and RLE encoders are byte-identical to
the C libraries the firmware and the kernel driver share (`rgb565-qoi` /
`rgb565-rle`).  If that is false, every "the encoder agrees with the device"
conclusion elsewhere is built on sand.

    python3 tests/test_encoder_reference.py

No device, no network, no numpy: the reference vectors are constants that live
in the C libraries and are mirrored by `pud_usb._REFERENCE_STREAM` /
`pud_usb._RLE_REFERENCE_STREAM`.  The device-side probe firmware that consumed
`DECODER_TYPE 6` is exercised through `pud_usb.qoid_encode`, whose sub-header
layout is pinned by the same kind of vector.
'''

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "common"))

from harness import Oracle, Report, Snapshot, Test, run_test  # noqa: E402
import pud_usb  # noqa: E402

# ORACLE: GOLDEN
# SOURCE: the C libraries' own reference vectors, mirrored in
#         tools/pud_usb.py as _REFERENCE_STREAM / _RLE_REFERENCE_STREAM
#         (rgb565-qoi / rgb565-rle); the QOI+dict sub-header layout is pinned
#         by _REFERENCE_QOID_KEYFRAME / _REFERENCE_QOID_DELTA
# EXPECTED: byte-for-byte equality, no tolerance
ORACLE = Oracle(
    "GOLDEN",
    "tools/pud_usb.py `_REFERENCE_STREAM` / `_RLE_REFERENCE_STREAM` / "
    "`_REFERENCE_QOID_*` (the C libraries' reference vectors)",
    "qoi_encode and rle_encode reproduce the C bytes exactly; the QOI+dict "
    "sub-header is exactly the 16 reference bytes and round-trips through zlib",
    "no tolerance: a single different byte is a failure")

PIXELS = (0x1234, 0x1235, 0x1236, 0x1236, 0x1236, 0xF800, 0x07E0, 0x1234)


def check(dev, args):
    snaps, ok = [], True

    packed = pud_usb.struct.pack("<%dH" % len(PIXELS), *PIXELS)

    qoi_tuple = pud_usb.qoi_encode(PIXELS)
    qoi_bytes = pud_usb.qoi_encode(packed)
    snaps.append(Snapshot("qoi_reference_bytes", len(qoi_tuple), "bytes"))
    if qoi_tuple != pud_usb._REFERENCE_STREAM:
        ok = False
        snaps.append(Snapshot("qoi_mismatch",
                              "%s != %s" % (qoi_tuple.hex(),
                                            pud_usb._REFERENCE_STREAM.hex())))
    if qoi_bytes != qoi_tuple:
        ok = False
        snaps.append(Snapshot("qoi_tuple_vs_bytes_path", "differ"))

    rle_tuple = pud_usb.rle_encode(PIXELS)
    rle_bytes = pud_usb.rle_encode(packed)
    snaps.append(Snapshot("rle_reference_bytes", len(rle_tuple), "bytes"))
    if rle_tuple != pud_usb._RLE_REFERENCE_STREAM:
        ok = False
        snaps.append(Snapshot("rle_mismatch",
                              "%s != %s" % (rle_tuple.hex(),
                                            pud_usb._RLE_REFERENCE_STREAM.hex())))
    if rle_bytes != rle_tuple:
        ok = False
        snaps.append(Snapshot("rle_tuple_vs_bytes_path", "differ"))

    # RGB565 packing: 24-bit RGB -> 16-bit, truncating (not rounding), which is
    # what makes the C and Python paths comparable at all.
    got = pud_usb.rgb888_to_rgb565(bytes([0, 0, 0, 255, 255, 255]), 2, 1)
    snaps.append(Snapshot("rgb565_black_white", got.hex()))
    if got != bytes([0x00, 0x00, 0xFF, 0xFF]):
        ok = False

    import zlib
    sub = pud_usb.QOID_SUBHEADER.pack(
        pud_usb.QOID_MAGIC, pud_usb.QOID_FLAG_KEYFRAME, 0, 0, 0)
    snaps.append(Snapshot("qoid_keyframe_subheader", sub.hex()))
    if sub != pud_usb._REFERENCE_QOID_KEYFRAME:
        ok = False

    sub = pud_usb.QOID_SUBHEADER.pack(
        pud_usb.QOID_MAGIC, pud_usb.QOID_FLAG_DELTA, 0, 5, 0x1b)
    snaps.append(Snapshot("qoid_delta_subheader", sub.hex()))
    if sub != pud_usb._REFERENCE_QOID_DELTA:
        ok = False

    blob = pud_usb.qoiz_encode(PIXELS)
    snaps.append(Snapshot("qoiz_bytes", len(blob), "bytes"))
    try:
        if zlib.decompress(blob, -15) != pud_usb._REFERENCE_STREAM:
            ok = False
            snaps.append(Snapshot("qoiz_round_trip", "does not inflate back"))
    except zlib.error as exc:
        ok = False
        snaps.append(Snapshot("qoiz_round_trip", "zlib error %s" % exc))

    detail = "all reference vectors reproduced" if ok else "see mismatches"
    return Report.of(snaps, ok=ok, detail=detail)


TEST = Test(
    "test_encoder_reference", ORACLE, check, needs_device=False,
    description="Python QOI/RLE encoders vs the C libraries' reference "
                "vectors (offline)")

if __name__ == "__main__":
    run_test(TEST)
