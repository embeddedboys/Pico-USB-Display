#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
The EP2 query channel answers with the exact byte counts the protocol
specifies, and the capability report is internally consistent with the panel
the firmware says it drives.

Layers, deliberately separated (a working channel is not the same as a correct
report):

  1. `PUD_CMD_GET_SN` returns exactly 8 bytes.
  2. `PUD_CMD_GET_CAPS` returns a report whose `magic` and `proto_ver` match the
     protocol, and whose `frame_max` is one of the two per-board transfer
     limits the firmware defines -- not a host-side clamp.
  3. `decoder_type` is a value the protocol documents, and this repo's default
     build reports QOI (3).
  4. The panel block is self-consistent: the geometry must match the rotation
     the device reports, using the same table as `notes/usb-protocol.md`.
  5. An unknown command must produce a short read (the device answers 0 bytes),
     which is how a host detects "this firmware does not have that command".

    python3 tests/test_ep2_query.py
    python3 tests/test_ep2_query.py --json
    python3 tests/test_ep2_query.py --unknown-cmd 0x7f   # the stale-buffer probe

Needs the device with the `pud` kernel driver unbound.  No device =>
ENVIRONMENT_ERROR (3), never FAIL.
'''

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "common"))
sys.path.insert(0, os.path.join(_HERE, "tools"))

from harness import Oracle, Report, Snapshot, Test, run_test  # noqa: E402
import pud_usb  # noqa: E402

# ORACLE: SPEC
# SOURCE: notes/usb-protocol.md -- the request table ("0x01 PUD_CMD_GET_SN ...
#         8 bytes", "0x02 PUD_CMD_GET_CAPS ... struct pud_caps 32 B", "others
#         return 0 => short read -> unsupported"), the rotation->geometry table
#         in the runtime-parameter section, and the per-board PUD_MAX_TRANSFER
#         values in src/cherryusb/usbd_vendor.h
# EXPECTED: exact byte counts; magic 0x43445550, proto 2; frame_max in
#           {65536, 32768}; decoder_type in 0..6 (default build: 3);
#           geometry consistent with the reported rotation; unknown command
#           short-reads
ORACLE = Oracle(
    "SPEC",
    "notes/usb-protocol.md (query table, caps struct, rotation->geometry "
    "table) + src/cherryusb/usbd_vendor.h (PUD_MAX_TRANSFER per board)",
    "SN 8 bytes; caps magic/proto exact, frame_max in {65536, 32768}; "
    "decoder_type 0..6 and 3 on this default build; geometry follows the "
    "rotation table; an unknown command short-reads")

FRAME_MAX_VALUES = (65536, 32768)
DECODER_TYPES = (0, 1, 2, 3, 4, 5, 6)
DEFAULT_DECODER = 3  # CMakeLists.txt set(DECODER_TYPE 3)


def extra_args(ap):
    ap.add_argument("--unknown-cmd", type=lambda v: int(v, 0), default=0x7F,
                    help="command number expected to be unhandled "
                         "(default 0x7f, the stale-buffer probe)")


def check(dev, args):
    snaps, failures = [], []

    # 1 -- GET_SN
    sn = dev.get_sn()
    snaps.append(Snapshot("sn_length", len(sn), "bytes"))
    if len(sn) != 8:
        failures.append("PUD_CMD_GET_SN returned %d bytes, spec says 8"
                        % len(sn))

    # 2 -- GET_CAPS framing
    #
    # query_caps() documents None as "the answer did not carry the caps magic,
    # so the conservative defaults were kept".  That is a real failure of this
    # check (the device did not answer the command), not an environment
    # problem, so it is reported as a failed expectation rather than allowed to
    # raise TypeError.
    caps = dev.query_caps()
    if caps is None:
        failures.append("PUD_CMD_GET_CAPS did not answer with the caps magic "
                        "0x%08x (query_caps() returned None and kept the "
                        "conservative defaults)" % pud_usb.CAPS_MAGIC)
        return Report.of(snaps, ok=False, detail="; ".join(failures))

    snaps.append(Snapshot("caps.magic", "0x%08x" % caps["magic"]))
    snaps.append(Snapshot("caps.proto_ver", caps["proto_ver"]))
    snaps.append(Snapshot("caps.frame_max", caps["frame_max"], "bytes"))
    snaps.append(Snapshot("caps.decoder_type", caps["decoder_type"]))
    if caps["magic"] != pud_usb.CAPS_MAGIC:
        failures.append("caps magic 0x%08x != 0x%08x"
                        % (caps["magic"], pud_usb.CAPS_MAGIC))
    if caps["proto_ver"] != pud_usb.PUD_PROTO_VER:
        failures.append("caps proto_ver %d != %d (host and device disagree "
                        "about the wire format)" % (caps["proto_ver"],
                                                    pud_usb.PUD_PROTO_VER))
    if caps["frame_max"] not in FRAME_MAX_VALUES:
        failures.append("caps frame_max %d is not one of the firmware's "
                        "per-board limits %s" % (caps["frame_max"],
                                                 list(FRAME_MAX_VALUES)))

    # 3 -- decoder_type is a documented value; the default build is QOI
    if caps["decoder_type"] not in DECODER_TYPES:
        failures.append("caps decoder_type %d is outside the documented range "
                        "0..6" % caps["decoder_type"])
    if args.expect_default_decoder and caps["decoder_type"] != DEFAULT_DECODER:
        failures.append("decoder_type %d but this repo's default build is %d; "
                        "pass --no-default-decoder if the firmware was built "
                        "with -DDECODER_TYPE" % (caps["decoder_type"],
                                                 DEFAULT_DECODER))

    # 4 -- panel geometry must agree with the rotation
    rot, xres, yres = caps["rotation"], caps["xres"], caps["yres"]
    snaps.append(Snapshot("caps.rotation", rot))
    snaps.append(Snapshot("caps.geometry", "%dx%d" % (xres, yres)))
    # Native panel is 320x480; rotations 0/2 are the native frame, 1/3 the
    # quarter-turned one (notes/usb-protocol.md's table).
    want = (320, 480) if rot % 2 == 0 else (480, 320)
    if (xres, yres) != want:
        failures.append("rotation %d should report %dx%d, device reports %dx%d"
                        % (rot, want[0], want[1], xres, yres))
    if caps["bpp"] != 16:
        failures.append("caps bpp %d, this panel is driven at 16 bpp"
                        % caps["bpp"])

    # 5 -- an unknown command must not be answered at all.  The device returns a
    # zero-length packet, so the host's bulk read comes back short -- pyusb
    # raises (timeout) or returns fewer bytes.  Either is the documented
    # "unsupported" signal; a full-length answer made of *anything* means the
    # device replied, which is the stale-buffer bug this probes for.
    echoed = False
    try:
        raw = dev.send_query(args.unknown_cmd, 32)
    except Exception as exc:
        # A raised transfer is the expected outcome: nothing was answered.
        snaps.append(Snapshot("unknown_cmd_response",
                              "raised %s" % type(exc).__name__))
    else:
        if len(raw) > 0:
            echoed = True
            snaps.append(Snapshot("unknown_cmd_response",
                                  "%d bytes echoed" % len(raw)))
        else:
            snaps.append(Snapshot("unknown_cmd_response", "empty answer"))
    snaps.append(Snapshot("unknown_cmd", "0x%02x" % args.unknown_cmd))
    if echoed:
        failures.append("cmd 0x%02x was answered with %d bytes; the protocol "
                        "says an unsupported command returns 0 bytes, and the "
                        "query buffer still holds the previous answer"
                        % (args.unknown_cmd, len(raw)))

    ok = not failures
    return Report.of(snaps, ok=ok,
                     detail="; ".join(failures) if failures
                     else "query channel and capability report are consistent")


def extra_args2(ap):
    extra_args(ap)
    ap.add_argument("--no-default-decoder", dest="expect_default_decoder",
                    action="store_false", default=True,
                    help="the firmware was built with -DDECODER_TYPE, so the "
                         "reported decoder need not be QOI (3)")


TEST = Test(
    "test_ep2_query", ORACLE, check,
    description="EP2 query channel and capability report consistency "
                "(needs device)")
TEST.extra_args = extra_args2

if __name__ == "__main__":
    run_test(TEST)
