#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
Exercise the runtime parameter channel (PUD_CMD_SET_PARAM / PUD_CMD_GET_PARAM).

This is the userspace half of the parameter protocol: it needs no kernel driver,
so it is the fastest way to check a firmware build.  The default run is a smoke
test --

  1. read the parameters and print them,
  2. dim the backlight to 10% and bring it back to 80% (watch the panel),
     verifying each write through a read-back -- the read-back is the level this
     script set; the panel itself is driven a few percent brighter, because the
     panel profile in pico-display-lib adds its own offset (bl_lvl_offs, 5 on
     this build) on top of it,
  3. ask for the fields this build cannot change yet (rotation, decoder)
     and check that they come back as "rejected" instead of silently ignored,

-- and exits non-zero if a write the device says it supports did not take, or if
a field it cannot do was reported as applied anyway.

Usage:
    ./scripts/param_test.py [--keep]        # --keep leaves the brightness at 80%
    ./scripts/param_test.py --brightness 30 # just set one field and read back
'''

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pud_usb


def show(tag, state):
    print("%-10s brightness=%3d%%  rotation=%d  decoder=%d   "
          "settable=%s  rejected=%s"
          % (tag, state["brightness"], state["rotation"],
             state["decoder"], pud_usb.param_names(state["settable"]),
             pud_usb.param_names(state["rejected"])))


def main():
    ap = argparse.ArgumentParser(description="runtime parameter channel test")
    ap.add_argument("--brightness", type=int, default=None,
                    help="set only this (percent) and print the read-back")
    ap.add_argument("--keep", action="store_true",
                    help="leave the backlight at the test level instead of "
                         "restoring what it was")
    args = ap.parse_args()

    try:
        with pud_usb.open_device() as disp:
            before = disp.get_params()
            show("before", before)

            if args.brightness is not None:
                disp.set_params(mask=pud_usb.PARAM_BRIGHTNESS,
                                brightness=args.brightness)
                after = disp.get_params()
                show("after", after)
                if after["rejected"]:
                    sys.exit("brightness is reported as unsupported: rejected=%s"
                             % pud_usb.param_names(after["rejected"]))
                if after["brightness"] != args.brightness:
                    sys.exit("read-back mismatch: asked %d, got %d"
                             % (args.brightness, after["brightness"]))
                print("ok")
                return

            # 1. the supported field: dim, check, restore
            for level in (10, 80):
                disp.set_params(mask=pud_usb.PARAM_BRIGHTNESS,
                                brightness=level)
                state = disp.get_params()
                show("brightness", state)
                if state["rejected"] & pud_usb.PARAM_BRIGHTNESS:
                    sys.exit("brightness came back as rejected")
                if state["brightness"] != level:
                    sys.exit("brightness read-back mismatch: asked %d, got %d"
                             % (level, state["brightness"]))

            # 2. the fields that are not implemented yet: they must be reported,
            #    not silently accepted
            want = pud_usb.PARAM_ROTATION | pud_usb.PARAM_DECODER
            disp.set_params(mask=want, rotation=(before["rotation"] + 1) % 4,
                            decoder=3)
            state = disp.get_params()
            show("rejected", state)
            if state["rejected"] != want:
                sys.exit("expected rejection of %s, got %s"
                         % (pud_usb.param_names(want),
                            pud_usb.param_names(state["rejected"])))
            if state["rotation"] != before["rotation"]:
                sys.exit("rotation changed although the build cannot set it")
            print("unsupported fields reported correctly: %s"
                  % pud_usb.param_names(state["rejected"]))

            # 3. put the backlight back the way we found it
            if not args.keep:
                disp.set_params(mask=pud_usb.PARAM_BRIGHTNESS,
                                brightness=before["brightness"])
                state = disp.get_params()
                show("restored", state)
                if state["brightness"] != before["brightness"]:
                    sys.exit("could not restore the backlight")

            print("ok")
    except pud_usb.PudError as exc:
        sys.exit(str(exc))


if __name__ == "__main__":
    main()
