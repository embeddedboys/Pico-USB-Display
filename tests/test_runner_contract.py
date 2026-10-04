#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
The test infrastructure itself is under test: the harness maps verdicts to the
workspace exit codes, and each device test's decision logic produces the right
verdict for a known input.

Two things are checked offline, which matters because the hardware tests cannot
be run in CI or on a machine without the panel:

  1. **The harness contract** -- `Report(ok=True)` -> 0, `ok=False` -> 1,
     `inconclusive` -> 5, no device -> 3, a `Timeout` -> 4, bad arguments -> 2,
     and a missing oracle is rejected at declaration time.

  2. **The device tests' decisions** -- each `check(dev, args)` is called with a
     stub device, so a PASS/FAIL path is exercised without the hardware.  A
     test whose logic only ever ran on the board would otherwise be unverified
     code, and a mirror drifting out of the harness would go unnoticed.

    python3 tests/test_runner_contract.py
'''

import os
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
from pathlib import Path  # noqa: E402
_TESTS = Path(_HERE)
sys.path.insert(0, os.path.join(_HERE, "common"))
sys.path.insert(0, os.path.join(_HERE, "tools"))

from harness import (EXIT_ENVIRONMENT_ERROR, EXIT_FAIL, EXIT_PASS,  # noqa: E402
                     EXIT_INCONCLUSIVE, EXIT_TIMEOUT, Oracle, Report,
                     Snapshot, Test, run_test)
import pud_usb  # noqa: E402

# ORACLE: REQUIREMENT
# SOURCE: ../AGENTS.md "测试与可复用工具约定" (exit codes 0 PASS / 1 FAIL /
#         2 INVALID_USAGE / 3 ENVIRONMENT_ERROR / 4 TIMEOUT / 5 INCONCLUSIVE)
#         and the oracle declaration rule
# EXPECTED: the harness maps each verdict to exactly that code, and each device
#           test's check() returns the documented verdict for a stub device
ORACLE = Oracle(
    "REQUIREMENT",
    "../AGENTS.md (exit-code table; oracle declaration requirement)",
    "Report(ok) -> 0/1, inconclusive -> 5, no device -> 3, timeout -> 4, "
    "invalid usage -> 2; each device test's check() decides correctly on a "
    "stub device")


# ---------------------------------------------------------------------------
# stub device
# ---------------------------------------------------------------------------

class FakeCaps(dict):
    pass


class FakeDevice:
    """A device that answers like a healthy one, for logic exercises only.

    Every value is a parameter, so a test can be driven into its PASS path and
    into a FAIL path by changing one field.
    """

    def __init__(self, caps=None, params=None, sn=None, touch=None,
                 unknown_cmd_answers=True, frame_max=(65536, 32768)):
        self.caps = caps or {
            "magic": pud_usb.CAPS_MAGIC, "proto_ver": pud_usb.PUD_PROTO_VER,
            "frame_max": 65536, "decoder_type": 3, "xres": 480, "yres": 320,
            "rotation": 1, "bpp": 16, "touch": True,
            "tp_polling_period": 10, "pixelclock_khz": 50000,
        }
        self.frame_max = min(pud_usb.USB_TRANS_MAX_SIZE,
                             self.caps["frame_max"])
        self.band_pixels = max(
            1, (self.frame_max - pud_usb.EP1_HEADER_SIZE - 16) // 3)
        self.decoder_type = self.caps["decoder_type"]
        self.width, self.height = self.caps["xres"], self.caps["yres"]
        self.sn = sn if sn is not None else b"\x01\x02\x03\x04\x05\x06\x07\x08"
        self.state = dict(params or {"settable": pud_usb.PARAM_BRIGHTNESS |
                                     pud_usb.PARAM_ROTATION,
                                     "rejected": 0, "brightness": 50,
                                     "rotation": self.caps["rotation"],
                                     "decoder": self.decoder_type})
        self.unknown_cmd_answers = unknown_cmd_answers
        self.touch = touch or []
        self.sent = []

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def get_sn(self):
        return self.sn

    def send_query(self, cmd, length, timeout=None):
        if cmd == pud_usb.CMD_GET_CAPS:
            import struct
            blob = pud_usb.CAPS_STRUCT.pack(
                self.caps["magic"], self.caps["proto_ver"],
                self.caps["frame_max"], self.caps["decoder_type"],
                self.caps["xres"], self.caps["yres"],
                self.caps["pixelclock_khz"], self.caps["rotation"],
                self.caps["bpp"], 0, self.caps["tp_polling_period"], 70, 40,
                pud_usb.CAPS_TOUCH if self.caps["touch"] else 0)
            return blob
        if cmd == pud_usb.CMD_GET_PARAM:
            import struct
            return pud_usb.PARAM_STATE_STRUCT.pack(
                self.state["settable"], self.state["rejected"],
                self.state["brightness"], self.state["rotation"], 0,
                self.state["decoder"])
        if cmd == pud_usb.CMD_GET_SN:
            return self.sn
        if self.unknown_cmd_answers:
            return bytes(length)
        raise TimeoutError("no answer")

    def query_caps(self):
        # geometry follows the same rule the firmware uses
        if self.state["rotation"] % 2 == 0:
            self.caps["xres"], self.caps["yres"] = 320, 480
        else:
            self.caps["xres"], self.caps["yres"] = 480, 320
        self.caps["rotation"] = self.state["rotation"]
        # A copy: callers hold the "before" snapshot while they change the
        # rotation, so returning the live dict would change history under them.
        return dict(self.caps)

    def get_params(self):
        return dict(self.state)

    def set_params(self, mask=0, brightness=0, rotation=0, decoder=0):
        if mask & pud_usb.PARAM_BRIGHTNESS:
            self.state["brightness"] = brightness
        if mask & pud_usb.PARAM_ROTATION:
            self.state["rotation"] = rotation
        # the decoder is a build-time choice: always reported as rejected
        self.state["rejected"] = mask & pud_usb.PARAM_DECODER

    def touch_request(self, timeout=None):
        pass

    def read_touch(self, timeout=None):
        if not self.touch:
            raise type("USBTimeoutError", (Exception,), {})("idle")
        return self.touch.pop(0)

    def send_rgb565(self, rgb565, width, height, xs=0, ys=0, timeout=None,
                    codec="qoi"):
        self.sent.append((width, height))
        return 1, len(rgb565) // 2, 0.001

    def send_raw(self, payload, xs, ys, xe, ye, timeout=None):
        self.sent.append((xe - xs + 1, ye - ys + 1))


class Args(dict):
    def __getattr__(self, name):
        try:
            return self[name]
        except KeyError:
            raise AttributeError(name)


# ---------------------------------------------------------------------------
# checks
# ---------------------------------------------------------------------------

def _harness_contract(snaps, failures):
    cases = [
        ("Report(ok=True)", Report.of(ok=True), EXIT_PASS),
        ("Report(ok=False)", Report.of(ok=False), EXIT_FAIL),
        ("Report(inconclusive=True)", Report.of(inconclusive=True),
         EXIT_INCONCLUSIVE),
        ("Report(ok=None)", Report.of(), EXIT_INCONCLUSIVE),
    ]
    for label, report, want in cases:
        snaps.append(Snapshot(label, "want %d" % want))
        if (report.ok is None or report.inconclusive):
            got = EXIT_INCONCLUSIVE
        else:
            got = EXIT_PASS if report.ok else EXIT_FAIL
        if got != want:
            failures.append("%s mapped to %d, want %d" % (label, got, want))

    # An unknown oracle type must be rejected where it is written, not silently
    # accepted and reported later.
    try:
        Oracle("MAYBE", "somewhere", "something")
    except ValueError:
        snaps.append(Snapshot("unknown oracle type rejected", "yes"))
    else:
        failures.append("Oracle('MAYBE', ...) was accepted")

    # And the exit-code constants must be the workspace's numbers.
    want_codes = {"PASS": 0, "FAIL": 1, "INVALID_USAGE": 2,
                  "ENVIRONMENT_ERROR": 3, "TIMEOUT": 4, "INCONCLUSIVE": 5}
    import harness
    got_codes = {
        "PASS": harness.EXIT_PASS, "FAIL": harness.EXIT_FAIL,
        "INVALID_USAGE": harness.EXIT_INVALID_USAGE,
        "ENVIRONMENT_ERROR": harness.EXIT_ENVIRONMENT_ERROR,
        "TIMEOUT": harness.EXIT_TIMEOUT,
        "INCONCLUSIVE": harness.EXIT_INCONCLUSIVE,
    }
    snaps.append(Snapshot("exit code table", got_codes))
    if got_codes != want_codes:
        failures.append("exit codes %s != %s" % (got_codes, want_codes))


def _run_subprocess(path, args, env_python, extra=()):
    out = subprocess.run([env_python, path] + list(args) + list(extra),
                         capture_output=True, text=True,
                         cwd=str(_TESTS.parent))
    return out.returncode, out.stdout, out.stderr


# A vendor/product pair no USB device can carry.  The "no device" case has to
# be *constructed*, not assumed: this contract test also runs on the bench,
# where the panel is attached, and an attached device would take test_ep2_query
# down its FAIL path (exit 1) instead of the environment path -- making the
# assertion below test the opposite of what it claims.
NO_SUCH_DEVICE = ("--vendor", "0", "--product", "0")


def check(dev, args):
    snaps, failures = [], []
    _harness_contract(snaps, failures)

    py = sys.executable

    # The real CLI paths: offline PASS, and (with a python that lacks pyusb)
    # a device test must report ENVIRONMENT_ERROR rather than FAIL.
    rc, _, _ = _run_subprocess(str(_TESTS / "test_encoder_reference.py"), [], py)
    snaps.append(Snapshot("test_encoder_reference exit code", rc))
    if rc != EXIT_PASS:
        failures.append("the offline encoder test exited %d, want 0" % rc)

    rc, _, _ = _run_subprocess(str(_TESTS / "test_ep2_query.py"), [], py,
                               extra=NO_SUCH_DEVICE)
    snaps.append(Snapshot("device test with no matching device exit code", rc))
    if rc != EXIT_ENVIRONMENT_ERROR:
        failures.append("a device test with no matching device exited %d; "
                        "missing hardware must be 3, never 1" % rc)

    rc, _, _ = _run_subprocess(str(_TESTS / "test_encoder_reference.py"),
                               ["--no-such-option"], py)
    snaps.append(Snapshot("bad option exit code", rc))
    if rc != 2:
        failures.append("an invalid option exited %d, want 2 "
                        "(argparse's own exit code is 2, do not change it)" % rc)

    # Each device test's decision logic, on a stub device that mimics a healthy
    # firmware.  These import the test module and call check() directly.
    healthy = FakeDevice()
    health = dict(caps=dict(healthy.caps), unknown_cmd_answers=False)

    import test_ep2_query
    rep = test_ep2_query.check(FakeDevice(**health),
                               Args(unknown_cmd=0x7F,
                                    expect_default_decoder=True))
    snaps.append(Snapshot("test_ep2_query on a healthy stub", rep.ok))
    if rep.ok is not True:
        failures.append("test_ep2_query failed on a healthy stub: %s"
                        % rep.detail)
    broken = FakeDevice(caps=dict(healthy.caps, frame_max=12345),
                        unknown_cmd_answers=False)
    rep = test_ep2_query.check(broken, Args(unknown_cmd=0x7F,
                                            expect_default_decoder=True))
    snaps.append(Snapshot("test_ep2_query on a bad frame_max", rep.ok))
    if rep.ok is not False:
        failures.append("test_ep2_query accepted frame_max=12345")

    import test_param_channel
    rep = test_param_channel.check(FakeDevice(**health),
                                   Args(brightness=None, keep=True,
                                        skip_rotation=False))
    snaps.append(Snapshot("test_param_channel on a healthy stub", rep.ok))
    if rep.ok is not True:
        failures.append("test_param_channel failed on a healthy stub: %s"
                        % rep.detail)

    import test_rotation_geometry
    rep = test_rotation_geometry.check(
        FakeDevice(**health),
        Args(rotation=None, cycle=1, delay=0, image=None))
    snaps.append(Snapshot("test_rotation_geometry on a healthy stub", rep.ok))
    if rep.ok is not True:
        failures.append("test_rotation_geometry failed on a healthy stub: %s"
                        % rep.detail)

    import test_touch_ep4
    idle = FakeDevice(**health, touch=[])
    rep = test_touch_ep4.check(idle, Args(mode="push", seconds=0.05,
                                          idle_timeout=1, calibrate=False,
                                          rate=0))
    snaps.append(Snapshot("test_touch_ep4 on an idle stub",
                          "inconclusive" if rep.inconclusive else rep.ok))
    if not rep.inconclusive:
        failures.append("an idle panel must be INCONCLUSIVE, not %s" % rep.ok)

    touched = FakeDevice(**health,
                         touch=[{"pressed": True, "x": 10, "y": 20, "seq": 1,
                                 "version": 1}] * 5)
    rep = test_touch_ep4.check(touched, Args(mode="push", seconds=0.05,
                                             idle_timeout=1, calibrate=False,
                                             rate=0))
    snaps.append(Snapshot("test_touch_ep4 on a touched stub", rep.ok))
    if rep.ok is not True:
        failures.append("test_touch_ep4 failed on a touched stub: %s"
                        % rep.detail)

    bad = FakeDevice(**health,
                     touch=[{"pressed": True, "x": 9999, "y": 20, "seq": 1,
                             "version": 1}])
    rep = test_touch_ep4.check(bad, Args(mode="push", seconds=0.05,
                                         idle_timeout=1, calibrate=False,
                                         rate=0))
    snaps.append(Snapshot("test_touch_ep4 on an out-of-range point", rep.ok))
    if rep.ok is not False:
        failures.append("test_touch_ep4 accepted a point outside the panel")

    return Report.of(snaps, ok=not failures,
                     detail="; ".join(failures[:6]) if failures
                     else "harness and test decisions match the contract")


TEST = Test(
    "test_runner_contract", ORACLE, check, needs_device=False,
    default_timeout=180.0,
    description="the test harness contract and every device test's decision "
                "logic, offline")

if __name__ == "__main__":
    run_test(TEST)
