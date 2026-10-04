#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
Protocol constants and build-switch defaults in the documentation and the
Python mirror must match the firmware source and CMakeLists.

The knowledge base is only trustworthy if its numbers are the code's numbers:
a mirror that says `PUD_CMD_GET_CAPS` is 0x02 while `include/pud.h` says
something else produces a wrong packet, and a `notes/` table that lists an old
default sends a reader down a path the build no longer takes.  This test reads
the values out of the source (`tools/pudctl.py src`'s reader) and compares them
with the mirror in `tools/pud_usb.py`, plus the handful of build-switch
defaults the notes quote.

One honest limitation: the reader does not evaluate the preprocessor.  A
platform-conditional constant gives the first textual definition, so
`PUD_MAX_TRANSFER` is checked as "one of the per-board values the source
declares", not as "the value this host would compile".

    python3 tests/test_protocol_constants.py

No device.  Needs the source tree and `tools/pud_usb.py` only.
'''

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "common"))
sys.path.insert(0, os.path.join(_HERE, os.pardir, "tools"))

from harness import Oracle, Report, Snapshot, Test, run_test  # noqa: E402
import pud_usb  # noqa: E402
import pudctl  # noqa: E402

REPO = os.path.normpath(os.path.join(_HERE, os.pardir))
PUD_H = os.path.join(REPO, "include", "pud.h")
VENDOR_H = os.path.join(REPO, "src", "cherryusb", "usbd_vendor.h")
CMAKE = os.path.join(REPO, "CMakeLists.txt")

# ORACLE: SPEC
# SOURCE: the firmware source itself -- include/pud.h and
#         src/cherryusb/usbd_vendor.h are the definition; tools/pud_usb.py is
#         the host mirror, and CMakeLists.txt holds the build-switch defaults
#         the notes quote
# EXPECTED: every mirrored constant equals the source value; every documented
#           build-switch default equals the CMakeLists.txt default
ORACLE = Oracle(
    "SPEC",
    "include/pud.h, src/cherryusb/usbd_vendor.h, CMakeLists.txt (the source "
    "of truth); tools/pud_usb.py and notes/ are the mirrors under test",
    "mirrored protocol constants equal the source values; quoted build-switch "
    "defaults equal CMakeLists.txt's CACHE defaults")

#: (mirror attribute in pud_usb, file, #define name).  These are the constants
#: that cross the repo boundary: a mismatch silently produces a wrong packet.
MIRRORED = [
    ("PUD_PROTO_VER", PUD_H, "PUD_PROTO_VER"),
    ("CAPS_MAGIC", PUD_H, "PUD_CAPS_MAGIC"),
    ("CMD_GET_SN", PUD_H, "PUD_CMD_GET_SN"),
    ("CMD_GET_CAPS", PUD_H, "PUD_CMD_GET_CAPS"),
    ("CMD_SET_PARAM", PUD_H, "PUD_CMD_SET_PARAM"),
    ("CMD_GET_PARAM", PUD_H, "PUD_CMD_GET_PARAM"),
    ("CMD_GET_QOID", PUD_H, "PUD_CMD_GET_QOID"),
    ("REQ_EP2_IN", VENDOR_H, "REQ_EP2_IN"),
    ("REQ_EP4_IN", VENDOR_H, "REQ_EP4_IN"),
    ("REQ_SET_PARAM", VENDOR_H, "REQ_SET_PARAM"),
    ("REQ_EP1_OUT", VENDOR_H, "REQ_EP1_OUT"),
    ("PARAM_BRIGHTNESS", PUD_H, "PUD_PARAM_BRIGHTNESS"),
    ("PARAM_ROTATION", PUD_H, "PUD_PARAM_ROTATION"),
    ("PARAM_DECODER", PUD_H, "PUD_PARAM_DECODER"),
    ("VID", VENDOR_H, "VENDOR_ID"),
    ("PID", VENDOR_H, "PRODUCT_ID"),
    ("CAPS_TOUCH", PUD_H, "PUD_CAPS_TOUCH"),
    ("QOID_STATE_MAGIC", PUD_H, "PUD_QOID_MAGIC"),
    ("QOID_WINDOWS", PUD_H, "PUD_QOID_WINDOWS"),
]

#: Host structs whose wire size the firmware states as a `#define PUD_*_SIZE`
#: next to a `_Static_assert`.  Both sides unpack a fixed layout, so a size
#: mismatch is a protocol break, not a style issue.
STRUCT_SIZES = [
    ("CAPS_STRUCT", PUD_H, "PUD_CAPS_SIZE"),
    ("PARAMS_STRUCT", PUD_H, "PUD_PARAMS_SIZE"),
    ("PARAM_STATE_STRUCT", PUD_H, "PUD_PARAM_STATE_SIZE"),
    ("QOID_STATE", PUD_H, "PUD_QOID_STATE_SIZE"),
]

#: Build switches the notes quote a default for.  (name, expected default,
#: where the notes say it).  A mismatch here is documentation drift, and the
#: CMakeLists default is what the project actually builds with.
BUILD_DEFAULTS = [
    ("DECODER_TYPE", "3", "notes/decoders.md, AGENTS.md 当前配置"),
    ("OVERCLOCK_ENABLED", "1", "AGENTS.md 当前配置"),
    ("PIO_USE_DMA", "1", "AGENTS.md 当前配置"),
    ("PUD_DECODER_PINGPONG", "1", "AGENTS.md 可调构建开关"),
    ("PUD_CODEC_IN_RAM", "1", "AGENTS.md 可调构建开关"),
    ("DECODER_STATS", "0", "AGENTS.md 可调构建开关"),
    ("PUD_INFLATE", "tinfl", "AGENTS.md 可调构建开关"),
    ("PUD_EP1_SINK", "0", "CMakeLists.txt 注释"),
]


def _cmake_cache_default(name):
    """The `set(NAME <value> ...)` default in the top-level CMakeLists.

    Accepts both the `CACHE STRING` form (a user-settable switch) and a plain
    `set(NAME 1)` override (how this repo pins a board profile), because the
    notes quote the effective value either way.
    """
    import re

    text = open(CMAKE, "r", encoding="utf-8").read()
    m = re.search(r"^set\(\s*" + re.escape(name) + r"\s+(\S+?)[\s\)]",
                  text, re.M)
    return m.group(1) if m else None


def check(dev, args):
    snaps, failures = [], []

    for attr, path, define in MIRRORED:
        src = pudctl.read_constant(path, define)
        mirror = getattr(pud_usb, attr, None)
        snaps.append(Snapshot("pud_usb.%s" % attr, mirror))
        if src is None:
            failures.append("source has no #define %s in %s"
                            % (define, os.path.basename(path)))
        elif mirror != src:
            failures.append("pud_usb.%s = %s but %s says %s"
                            % (attr, mirror, os.path.basename(path), src))

    # struct sizes: the format string's own .size is what the wire sees
    for attr, path, define in STRUCT_SIZES:
        mirror = getattr(pud_usb, attr, None)
        size = mirror.size if hasattr(mirror, "size") else mirror
        snaps.append(Snapshot("pud_usb.%s.size" % attr, size, "bytes"))
        declared = pudctl.read_constant(path, define)
        if declared is None:
            failures.append("%s has no #define %s to check %s against"
                            % (os.path.basename(path), define, attr))
            continue
        if size != declared:
            failures.append("pud_usb.%s is %d bytes, firmware asserts %d"
                            % (attr, size, declared))

    for name, expected, where in BUILD_DEFAULTS:
        got = _cmake_cache_default(name)
        snaps.append(Snapshot("CMakeLists.%s" % name, got))
        if got != expected:
            failures.append("CMakeLists.txt %s default is %r, notes quote %r (%s)"
                            % (name, got, expected, where))

    # PUD_MAX_TRANSFER is per board: the source declares two values and the
    # notes must quote both.  The reader does not evaluate the preprocessor, so
    # this checks that the pair is present rather than which one is active.
    import re
    vendor = open(VENDOR_H, "r", encoding="utf-8").read()
    block = vendor.split("PUD_MAX_TRANSFER", 1)[1][:400]
    board_values = sorted(int(v) * 1024
                          for v in re.findall(r"\((\d+) \* 1024\)", block))
    snaps.append(Snapshot("PUD_MAX_TRANSFER per board", board_values, "bytes"))
    if 64 * 1024 not in board_values or 32 * 1024 not in board_values:
        failures.append("PUD_MAX_TRANSFER should declare 64 KB and 32 KB, "
                        "got %s" % board_values)

    ok = not failures
    return Report.of(snaps, ok=ok,
                     detail="; ".join(failures) if failures
                     else "mirrors and quoted defaults match the source")


TEST = Test(
    "test_protocol_constants", ORACLE, check, needs_device=False,
    description="protocol constants and quoted build-switch defaults vs the "
                "firmware source (offline)")

if __name__ == "__main__":
    run_test(TEST)
