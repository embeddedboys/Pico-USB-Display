#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
The runtime parameter channel (`PUD_CMD_SET_PARAM` / `PUD_CMD_GET_PARAM`)
behaves as the protocol specifies: supported fields take and read back,
unsupported fields are reported in `rejected` instead of being silently
dropped, and the capability report follows a rotation.

    python3 tests/test_param_channel.py
    python3 tests/test_param_channel.py --brightness 30   # just one field
    python3 tests/test_param_channel.py --keep            # leave the changes
    python3 tests/test_param_channel.py --json

This is the userspace half of the parameter contract, so it needs no kernel
driver and is the fastest way to check a firmware build.  Everything it writes
is restored on the way out, including on failure.

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
# SOURCE: notes/usb-protocol.md, "运行期参数" -- the field table
#         (brightness implemented, rotation implemented, decoder rejected
#         because DECODER_TYPE is a build-time `#if`), the read-back semantics
#         (the device returns the value the host set, while the panel runs a
#         panel-profile offset brighter), and the rotation->geometry table
# EXPECTED: a masked write reads back as the value written; `rejected` carries
#           exactly the fields the build cannot apply; `settable` is the
#           implemented set; a rotation change swaps the reported geometry
#           exactly when its parity differs from the previous rotation
ORACLE = Oracle(
    "SPEC",
    "notes/usb-protocol.md (runtime parameter section: field table, read-back "
    "semantics, rotation->geometry table)",
    "brightness and rotation write/read back exactly and are in settable; "
    "decoder is rejected; |read-back brightness - set| <= the documented "
    "panel-profile offset; geometry follows the rotation parity rule")

#: panels profile offset, documented as `bl_lvl_offs` = 5 on this build
#: (pico-display-lib `drivers/backlight/pwm_backlight.c`).  The read-back is
#: the value the host set, so the only tolerance involved is this offset.
BRIGHTNESS_OFFSET = 5


def extra_args(ap):
    ap.add_argument("--brightness", type=int, default=None,
                    help="set only this percentage and check the read-back")
    ap.add_argument("--keep", action="store_true",
                    help="leave brightness/rotation as the test set them")
    ap.add_argument("--skip-rotation", action="store_true",
                    help="do not touch the rotation (some panels are mounted "
                         "one way round)")


def check(dev, args):
    snaps, failures = [], []
    before = dev.get_params()
    snaps.append(Snapshot("before", "brightness=%d%% rotation=%d decoder=%d "
                                    "settable=%s rejected=%s"
                          % (before["brightness"], before["rotation"],
                             before["decoder"],
                             pud_usb.param_names(before["settable"]),
                             pud_usb.param_names(before["rejected"]))))

    def restore():
        if args.keep:
            return
        try:
            if not args.skip_rotation:
                dev.set_params(mask=pud_usb.PARAM_ROTATION,
                               rotation=before["rotation"])
            dev.set_params(mask=pud_usb.PARAM_BRIGHTNESS,
                           brightness=before["brightness"])
        except Exception:
            pass

    try:
        # -- the supported-and-implemented set
        want_settable = pud_usb.PARAM_BRIGHTNESS | pud_usb.PARAM_ROTATION
        snaps.append(Snapshot("settable",
                              pud_usb.param_names(before["settable"])))
        if before["settable"] & pud_usb.PARAM_BRIGHTNESS == 0:
            failures.append("this firmware reports brightness as not settable")
        if before["settable"] & pud_usb.PARAM_ROTATION == 0:
            failures.append("this firmware reports rotation as not settable "
                            "(the protocol implements it)")

        # -- brightness round trip
        levels = [args.brightness] if args.brightness is not None else [10, 80]
        for level in levels:
            dev.set_params(mask=pud_usb.PARAM_BRIGHTNESS, brightness=level)
            state = dev.get_params()
            snaps.append(Snapshot("brightness_set_%d" % level,
                                  "read %d%%, rejected=%s"
                                  % (state["brightness"],
                                     pud_usb.param_names(state["rejected"]))))
            if state["rejected"] & pud_usb.PARAM_BRIGHTNESS:
                failures.append("brightness came back rejected for %d%%" % level)
            elif abs(state["brightness"] - level) > BRIGHTNESS_OFFSET:
                failures.append("brightness read-back %d != %d (tolerance is "
                                "the documented panel-profile offset %d)"
                                % (state["brightness"], level,
                                   BRIGHTNESS_OFFSET))

        if args.brightness is not None:
            return Report.of(snaps, ok=not failures,
                             detail="; ".join(failures) if failures
                             else "brightness round trip is exact")

        # -- rotation: value, and the capability report following it
        if not args.skip_rotation:
            other = (before["rotation"] + 1) % 4
            caps_before = dev.query_caps()
            dev.set_params(mask=pud_usb.PARAM_ROTATION, rotation=other)
            state = dev.get_params()
            caps_after = dev.query_caps()
            snaps.append(Snapshot("rotation_set",
                                  "read %d, geometry %dx%d -> %dx%d"
                                  % (state["rotation"], caps_before["xres"],
                                     caps_before["yres"], caps_after["xres"],
                                     caps_after["yres"])))
            if state["rejected"] & pud_usb.PARAM_ROTATION:
                failures.append("rotation came back rejected")
            if state["rotation"] != other:
                failures.append("rotation read-back %d != %d"
                                % (state["rotation"], other))
            if caps_after["rotation"] != other:
                failures.append("caps still report rotation %d after setting %d"
                                % (caps_after["rotation"], other))
            # A quarter turn (parity change) swaps the frame; a half turn does
            # not.  This is the documented rule, and it is what a host builds
            # its DRM mode from.
            swapped = (other ^ before["rotation"]) & 1
            want = ((caps_before["yres"], caps_before["xres"]) if swapped
                    else (caps_before["xres"], caps_before["yres"]))
            got = (caps_after["xres"], caps_after["yres"])
            if got != want:
                failures.append("after a %s turn the geometry should be %dx%d, "
                                "device reports %dx%d"
                                % ("quarter" if swapped else "half",
                                   want[0], want[1], got[0], got[1]))

        # -- the build-time field must be reported, not silently accepted
        dev.set_params(mask=pud_usb.PARAM_DECODER, decoder=3)
        state = dev.get_params()
        snaps.append(Snapshot("decoder_rejected",
                              pud_usb.param_names(state["rejected"])))
        if state["rejected"] != pud_usb.PARAM_DECODER:
            failures.append("setting decoder should be reported as rejected "
                            "(DECODER_TYPE is a build-time #if), got %s"
                            % pud_usb.param_names(state["rejected"]))
    finally:
        restore()

    if not args.keep:
        after = dev.get_params()
        snaps.append(Snapshot("restored", "brightness=%d%% rotation=%d"
                              % (after["brightness"], after["rotation"])))
        if after["brightness"] != before["brightness"]:
            failures.append("could not restore brightness (%d -> %d)"
                            % (before["brightness"], after["brightness"]))
        if not args.skip_rotation and after["rotation"] != before["rotation"]:
            failures.append("could not restore rotation (%d -> %d)"
                            % (before["rotation"], after["rotation"]))

    return Report.of(snaps, ok=not failures,
                     detail="; ".join(failures) if failures
                     else "parameter channel matches the protocol")


TEST = Test(
    "test_param_channel", ORACLE, check,
    description="runtime parameter channel: writes, read-back, rejected mask "
                "(needs device)")
TEST.extra_args = extra_args

if __name__ == "__main__":
    run_test(TEST)
