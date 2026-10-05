#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
The one place every test in `tests/` gets its CLI, exit codes, oracle
declaration and device handling from.

A test is a *decision*, not a script: it collects observations with `tools/`
(`pud_usb.py`, `pudctl.py`), declares where its expected values come from, and
lets this module turn the comparison into an exit code.  Keeping that here is
what stops the next test from inventing its own conventions.

Conventions (workspace-wide, see `../AGENTS.md`):

    exit code   0 PASS    1 FAIL    2 INVALID_USAGE
                3 ENVIRONMENT_ERROR     4 TIMEOUT     5 INCONCLUSIVE

Every test declares its expectation next to the code that checks it:

    ORACLE = Oracle("SPEC", "notes/usb-protocol.md", "8 bytes",
                    "board unique id length")

    def check(dev, args):
        sn = dev.get_sn()
        return Report.of(Snapshot("unique id length", len(sn), "bytes"),
                         ok=(len(sn) == 8))

`ORACLE NONE` is not a failure: the test collects its observations, prints
them, and exits 5 (INCONCLUSIVE).  A missing device is never a FAIL either --
it is 3 (ENVIRONMENT_ERROR).  Both mappings live here, not in the tests.

    python3 tests/test_x.py --help

**这一份有两处副本，改动要两边同步**（像 `skills/` 那样，用 `diff` 校验为空）：
`Pico-USB-Display/tests/common/harness.py` 与
`pico_dm_qd3503728_esp32p4_idf/wireless/p4_wireless_display/tests/common/harness.py`。
'''

import argparse
import json
import os
import signal
import sys
import traceback
from dataclasses import dataclass, field

# tools/pud_usb.py lives one level up from tests/common/
_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOLS = os.path.normpath(os.path.join(_HERE, os.pardir, os.pardir, "tools"))
if _TOOLS not in sys.path:
    sys.path.insert(0, _TOOLS)

try:
    import pud_usb  # noqa: E402  (path set up above)
except ImportError:      # 不需要 USB 设备访问的测试（needs_device=False）
    pud_usb = None       # 项目里没有 tools/pud_usb.py 时也能用这套 CLI/退出码/oracle

EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_INVALID_USAGE = 2
EXIT_ENVIRONMENT_ERROR = 3
EXIT_TIMEOUT = 4
EXIT_INCONCLUSIVE = 5

STATUS_BY_CODE = {
    EXIT_PASS: "PASS",
    EXIT_FAIL: "FAIL",
    EXIT_INVALID_USAGE: "INVALID_USAGE",
    EXIT_ENVIRONMENT_ERROR: "ENVIRONMENT_ERROR",
    EXIT_TIMEOUT: "TIMEOUT",
    EXIT_INCONCLUSIVE: "INCONCLUSIVE",
}

VERSION = "1.0.0"

ORACLE_TYPES = ("SPEC", "REQUIREMENT", "INVARIANT", "RELATIONSHIP", "GOLDEN",
                "BASELINE", "USER_DEFINED", "NONE")


# ---------------------------------------------------------------------------
# Oracle declaration
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Oracle:
    """Where this test's expected values come from.

    type      SPEC / REQUIREMENT / INVARIANT / RELATIONSHIP / GOLDEN /
              BASELINE / USER_DEFINED / NONE
    source    the document, spec or artifact the expectation is read from
    expected  human-readable statement of the criterion
    note      optional caveat (why a NONE oracle, what would make it SPEC)
    """

    type: str
    source: str
    expected: str
    note: str = ""

    def __post_init__(self):
        if self.type not in ORACLE_TYPES:
            raise ValueError("unknown oracle type %r (want one of %s)"
                             % (self.type, ", ".join(ORACLE_TYPES)))

    def as_dict(self):
        return {"type": self.type, "source": self.source,
                "expected": self.expected, "note": self.note}


ORACLE_NONE = Oracle("NONE", "", "", "observation only; no reliable oracle")


class Timeout(Exception):
    pass


class Inconclusive(Exception):
    """The run cannot reach a verdict for a non-failure reason."""


class EnvironmentProblem(Exception):
    """Missing hardware, permission or dependency.  Not a test failure."""


# ---------------------------------------------------------------------------
# Observation
# ---------------------------------------------------------------------------

@dataclass
class Snapshot:
    """One measured value with an explicit unit."""

    name: str
    value: object
    unit: str = ""


@dataclass
class Report:
    """What a test returns: observations plus its verdict.

    `ok=None` with `inconclusive=False` is treated as INCONCLUSIVE by the
    runner, never as a pass.
    """

    observations: list = field(default_factory=list)
    ok: bool = None
    detail: str = ""
    inconclusive: bool = False

    @staticmethod
    def of(*snapshots, ok=None, detail="", inconclusive=False):
        flat = []
        for s in snapshots:
            if isinstance(s, (list, tuple)):
                flat.extend(s)
            else:
                flat.append(s)
        return Report(flat, ok, detail, inconclusive)

    def as_dict(self):
        obs = {}
        for s in self.observations:
            obs[s.name] = ({"value": s.value, "unit": s.unit} if s.unit
                           else s.value)
        return {"observation": obs}


# ---------------------------------------------------------------------------
# Test definition
# ---------------------------------------------------------------------------

class Test:
    """A named test: an oracle plus a function from device to Report.

    `needs_device=False` marks a purely offline test -- it still gets the
    unified CLI, exit codes and JSON, but never opens the device.
    """

    def __init__(self, name, oracle, fn, needs_device=True, version=VERSION,
                 description="", default_timeout=120.0, extra_args=None):
        self.name = name
        self.oracle = oracle
        self.fn = fn
        self.needs_device = needs_device
        self.version = version
        self.description = description or (fn.__doc__ or "").strip()
        self.default_timeout = default_timeout
        self.extra_args = extra_args


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _add_common_options(ap, default_timeout, needs_device=True):
    ap.add_argument("--version", action="version",
                    version="%(prog)s " + VERSION)
    ap.add_argument("--json", action="store_true",
                    help="emit one machine-readable JSON object")
    ap.add_argument("--quiet", action="store_true",
                    help="suppress the observation block")
    ap.add_argument("--verbose", action="store_true",
                    help="per-step diagnostics")
    ap.add_argument("--timeout", type=float, default=default_timeout,
                    help="whole-run timeout in seconds (default %g)"
                         % default_timeout)
    if needs_device and pud_usb is not None:
        ap.add_argument("--device-timeout", type=float, default=None,
                        help="per-transfer timeout in ms (pud_usb default)")
        ap.add_argument("--vendor", type=lambda v: int(v, 0), default=pud_usb.VID,
                        help="USB vendor id (default 0x%04x)" % pud_usb.VID)
        ap.add_argument("--product", type=lambda v: int(v, 0), default=pud_usb.PID,
                        help="USB product id (default 0x%04x)" % pud_usb.PID)


def _alarm_handler(signum, frame):
    raise Timeout("the run exceeded --timeout")


def _status_of(report):
    if report.inconclusive or report.ok is None:
        return "INCONCLUSIVE"
    return "PASS" if report.ok else "FAIL"


def _emit(test, report, extra, code=None):
    # 状态必须与退出码一致（这一层就是为一致性存在的）：runner 已经定了码的场合
    # （环境问题/超时/用法错）按码报，没定才由 report 推。踩过：没接板子时退出码 3
    # 但打印 INCONCLUSIVE ✗。
    status = STATUS_BY_CODE.get(code) if code is not None else _status_of(report)

    if extra.get("json"):
        payload = {"test": test.name, "status": status,
                   "oracle": test.oracle.as_dict(),
                   "test_version": test.version}
        payload.update(report.as_dict())
        payload.update({k: v for k, v in extra.items() if k != "json"})
        if report.detail:
            payload["detail"] = report.detail
        print(json.dumps(payload, indent=2, default=str))
        return

    if not extra.get("quiet"):
        print("[TEST] %s" % test.name)
        for s in report.observations:
            print("[MEASURE] %s = %s%s"
                  % (s.name, s.value, (" " + s.unit) if s.unit else ""))
        print("[ORACLE] %s" % test.oracle.type)
        if test.oracle.source:
            print("[SOURCE] %s" % test.oracle.source)
        if test.oracle.expected:
            print("[EXPECTED] %s" % test.oracle.expected)
        for k, v in extra.items():
            if k in ("json", "quiet", "verbose"):
                continue
            print("[%s] %s" % (k.upper(), v.replace("\n", "\n          ")))
        print()
    if report.detail:
        print("[DETAIL] %s" % report.detail)
    print("[RESULT] %s" % status)


def _run(test, args):
    """Run the test and return the exit code (reporting included)."""
    report = None
    code = None
    try:
        if not test.needs_device:
            report = test.fn(None, args)
        elif pud_usb is None:
            print("[ENVIRONMENT] this project has no tools/pud_usb.py",
                  file=sys.stderr)
            code = EXIT_ENVIRONMENT_ERROR
        else:
            try:
                dev = pud_usb.open_device(vid=args.vendor, pid=args.product)
            except pud_usb.PudError as exc:
                print("[ENVIRONMENT] %s" % exc, file=sys.stderr)
                code = EXIT_ENVIRONMENT_ERROR
            else:
                with dev:
                    report = test.fn(dev, args)
    except Timeout as exc:
        print("[TIMEOUT] %s" % exc, file=sys.stderr)
        report = Report.of(inconclusive=True, detail=str(exc))
        code = EXIT_TIMEOUT
    except ((pud_usb.PudError if pud_usb else EnvironmentProblem),
            EnvironmentProblem) as exc:
        print("[ENVIRONMENT] %s" % exc, file=sys.stderr)
        report = Report.of(inconclusive=True, detail=str(exc))
        code = EXIT_ENVIRONMENT_ERROR
    except Inconclusive as exc:
        report = Report.of(inconclusive=True, detail=str(exc))
        code = EXIT_INCONCLUSIVE
    except KeyboardInterrupt:
        print("[ENVIRONMENT] interrupted", file=sys.stderr)
        report = Report.of(inconclusive=True, detail="interrupted")
        code = EXIT_ENVIRONMENT_ERROR
    except BaseException as exc:
        # Anything unexpected is an environment problem, never a device FAIL.
        # SystemExit is re-raised: it is how a future inner helper bails out.
        if isinstance(exc, SystemExit):
            raise
        traceback.print_exc()
        report = Report.of(inconclusive=True, detail="unexpected error")
        code = EXIT_ENVIRONMENT_ERROR
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)

    if report is None:
        report = Report.of(inconclusive=True, detail="no verdict")
    if code is None:
        code = (EXIT_INCONCLUSIVE if (report.inconclusive or report.ok is None)
                else EXIT_PASS if report.ok else EXIT_FAIL)

    _emit(test, report, {"json": args.json}, code)
    return code


def run_test(test, argv=None):
    """Standard entry point: parse args, run, print, exit with the code."""
    ap = argparse.ArgumentParser(
        description=test.description or test.name,
        epilog="exit codes: 0 PASS, 1 FAIL, 2 INVALID_USAGE, "
               "3 ENVIRONMENT_ERROR, 4 TIMEOUT, 5 INCONCLUSIVE")
    _add_common_options(ap, test.default_timeout, test.needs_device)
    if test.extra_args:
        test.extra_args(ap)
    args = ap.parse_args(argv)

    if args.timeout and args.timeout > 0:
        signal.signal(signal.SIGALRM, _alarm_handler)
        signal.setitimer(signal.ITIMER_REAL, args.timeout)

    # One exit, at the end: _run() reports and returns the code, and only here
    # does it become the process status.
    sys.exit(_run(test, args))


__all__ = [
    "EXIT_PASS", "EXIT_FAIL", "EXIT_INVALID_USAGE", "EXIT_ENVIRONMENT_ERROR",
    "EXIT_TIMEOUT", "EXIT_INCONCLUSIVE", "STATUS_BY_CODE", "ORACLE_TYPES",
    "Oracle", "ORACLE_NONE", "Snapshot", "Report", "Test", "run_test",
    "Timeout", "Inconclusive", "EnvironmentProblem", "pud_usb", "VERSION",
]
