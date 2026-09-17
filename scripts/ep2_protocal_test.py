#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
Exercise the EP2 query channel.

The host sends a 4-byte request (u16 cmd, u16 size, little-endian) as a
vendor control request, then reads the reply from the EP2 bulk IN endpoint.
The only command implemented today is CMD_GET_SN = 0x01, which returns the
8-byte board unique id.

Usage:
    ./scripts/ep2_protocal_test.py [--repeat N] [--raw]

Exit status is non-zero if the device is missing or a reply has the wrong
length, so this is usable as a smoke test in a script.
'''

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pud_usb


def main():
    ap = argparse.ArgumentParser(description="EP2 query channel test")
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--raw", action="store_true", help="print raw bytes")
    args = ap.parse_args()

    try:
        with pud_usb.open_device() as disp:
            for _ in range(args.repeat):
                t0 = time.perf_counter()
                sn = disp.get_sn()
                dt = (time.perf_counter() - t0) * 1e3

                if len(sn) != 8:
                    sys.exit("bad reply length: got %d, want 8" % len(sn))

                printable = "".join(chr(c) if 32 <= c < 127 else "." for c in sn)
                print("sn: 0x%s  '%s'  (%.1f ms)%s"
                      % (sn.hex(), printable, dt,
                         "  raw=" + str(list(sn)) if args.raw else ""))
    except pud_usb.PudError as exc:
        sys.exit(str(exc))


if __name__ == "__main__":
    main()
