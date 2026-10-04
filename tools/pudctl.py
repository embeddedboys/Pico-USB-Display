#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
pudctl -- the reusable host-side tool for the PUD panel.

One entry point for the facts a test needs, so that no test has to open the
device, decode a descriptor or interpret a capability report itself:

    pudctl probe                 is the panel there, and what does it claim?
    pudctl caps                  the PUD_CMD_GET_CAPS report, as JSON
    pudctl params [--set ...]    read (and optionally write) the runtime params
    pudctl sn                    the 8-byte board unique id
    pudctl touch --mode push|poll  read EP4 touch reports for --seconds
    pudctl meter                 EP1 throughput sweep (see tools/measure.py)
    pudctl selftest              the offline encoder reference vectors

It only produces facts -- no thresholds, no PASS/FAIL.  Tests in `tests/` own
the oracles; this tool is what they measure with (and what a human uses to look
at the device by hand).

Conventions (workspace-wide): `pudctl <subcommand> [options]`, `--help`,
`--version`, `--json`, `--timeout`, `--quiet`, `--verbose`; exit codes
0 ok, 1 failure, 2 INVALID_USAGE, 3 ENVIRONMENT_ERROR, 4 TIMEOUT,
5 INCONCLUSIVE.  Here 1/5 mostly do not apply -- it reports facts -- but the
codes are shared so a caller can treat every tool the same way.
'''

import argparse
import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import pud_usb  # noqa: E402
import measure  # noqa: E402

VERSION = "1.0.0"

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_INVALID_USAGE = 2
EXIT_ENVIRONMENT_ERROR = 3
EXIT_TIMEOUT = 4
EXIT_INCONCLUSIVE = 5


def _print(args, human_lines, payload):
    if args.json:
        print(json.dumps(payload, indent=2, default=str))
    elif not args.quiet:
        for line in human_lines:
            print(line)


def cmd_probe(args):
    """Report whether the device is present and what it advertises."""
    try:
        dev = pud_usb.open_device(vid=args.vendor, pid=args.product)
    except pud_usb.PudError as exc:
        if args.json:
            print(json.dumps({"present": False, "error": str(exc)}, indent=2))
        else:
            print("absent: %s" % exc, file=sys.stderr)
        return EXIT_ENVIRONMENT_ERROR
    with dev:
        caps = dev.caps
        payload = {"present": True, "vid": "0x%04x" % args.vendor,
                   "pid": "0x%04x" % args.product, "caps": caps,
                   "band_pixels": dev.band_pixels}
        _print(args, ["present: %04x:%04x" % (args.vendor, args.product),
                      "caps: %s" % caps], payload)
    return EXIT_OK


def cmd_caps(args):
    """Print the PUD_CMD_GET_CAPS report."""
    with pud_usb.open_device(vid=args.vendor, pid=args.product) as dev:
        caps = dev.query_caps()
        _print(args, ["%s = %s" % (k, v) for k, v in sorted(caps.items())],
               {"caps": caps})
    return EXIT_OK


def cmd_params(args):
    """Read the runtime parameter state, optionally writing one field first.

    `--set brightness=NN`, `--set rotation=N`; the field is masked so only
    that one is touched.  Prints the state read back afterwards, which is the
    value the device reports -- not an assumption.
    """
    with pud_usb.open_device(vid=args.vendor, pid=args.product) as dev:
        before = dev.get_params()
        wrote = None
        for item in args.set or []:
            if "=" not in item:
                print("--set wants field=value, got %r" % item,
                      file=sys.stderr)
                return EXIT_INVALID_USAGE
            field, value = item.split("=", 1)
            value = int(value, 0)
            if field == "brightness":
                dev.set_params(mask=pud_usb.PARAM_BRIGHTNESS,
                               brightness=value)
            elif field == "rotation":
                dev.set_params(mask=pud_usb.PARAM_ROTATION, rotation=value)
            else:
                print("--set supports brightness and rotation, not %r"
                      % field, file=sys.stderr)
                return EXIT_INVALID_USAGE
            wrote = dict(wrote or {}, **{field: value})
            before = dev.get_params()
        state = dev.get_params()
        payload = {"wrote": wrote, "state": state}
        _print(args, ["brightness=%d%% rotation=%d decoder=%d"
                      % (state["brightness"], state["rotation"],
                         state["decoder"]),
                      "settable=%s rejected=%s"
                      % (pud_usb.param_names(state["settable"]),
                         pud_usb.param_names(state["rejected"]))], payload)
    return EXIT_OK


def cmd_sn(args):
    """Read the 8-byte board unique id."""
    with pud_usb.open_device(vid=args.vendor, pid=args.product) as dev:
        sn = dev.get_sn()
        payload = {"sn": sn.hex(), "bytes": list(sn), "length": len(sn)}
        _print(args, ["sn: %s" % sn.hex()], payload)
    return EXIT_OK


def cmd_touch(args):
    """Collect EP4 touch reports for --seconds and summarize them.

    Observation only: it prints how many reports arrived, their coordinate
    ranges and the median report interval.  Whether that is correct is the
    test's business (tests/test_touch_ep4.py).
    """
    import statistics
    import time

    reports, xs, ys, gaps, seqs = 0, [], [], [], []
    last_t = None
    with pud_usb.open_device(vid=args.vendor, pid=args.product) as dev:
        deadline = time.perf_counter() + args.seconds
        while time.perf_counter() < deadline:
            try:
                if args.mode == "poll":
                    dev.touch_request()
                rep = dev.read_touch(timeout=int(args.device_timeout or 500))
            except Exception as exc:
                if type(exc).__name__ != "USBTimeoutError":
                    raise
                if args.mode == "poll":
                    time.sleep(args.idle_timeout / 1e3)
                continue
            now = time.perf_counter()
            if last_t is not None:
                gaps.append((now - last_t) * 1e3)
            last_t = now
            reports += 1
            seqs.append(rep["seq"])
            if rep["pressed"]:
                xs.append(rep["x"])
                ys.append(rep["y"])
            if args.mode == "poll":
                time.sleep(args.idle_timeout / 1e3)

    payload = {
        "mode": args.mode, "seconds": args.seconds, "reports": reports,
        "x_range": [min(xs), max(xs)] if xs else None,
        "y_range": [min(ys), max(ys)] if ys else None,
        "gap_ms_median": statistics.median(gaps) if gaps else None,
        "sequences": [seqs[0], seqs[-1]] if seqs else None,
    }
    lines = ["reports: %d" % reports]
    if xs:
        lines.append("x range: %d..%d" % (min(xs), max(xs)))
        lines.append("y range: %d..%d" % (min(ys), max(ys)))
    if gaps:
        lines.append("report gap: median %.1f ms, min %.1f, max %.1f"
                     % (statistics.median(gaps), min(gaps), max(gaps)))
    _print(args, lines, payload)
    return EXIT_OK


def cmd_meter(args):
    """EP1 throughput sweep: full-width bands of increasing height.

    Payloads are encoded once, outside the timer (tools/measure.py), and the
    result is a table of observations.  There is deliberately no verdict here:
    whether a number is acceptable is defined by the test that reads it.
    """
    rows = []
    with pud_usb.open_device(vid=args.vendor, pid=args.product) as dev:
        for height in args.heights:
            if height > args.yres:
                continue
            rgb565 = measure.deterministic_rgb565(args.xres, height)
            rects = measure.band_rects(dev.band_pixels, args.xres, height)
            bands = measure.encode_bands(rgb565, args.xres, rects,
                                         codec=args.codec)
            r = measure.meter(dev, bands, repeats=args.repeats,
                              gap_ms=args.gap_ms)
            r["height"] = height
            rows.append(r)

    payload = {"xres": args.xres, "yres": args.yres, "codec": args.codec,
               "rows": [{k: v for k, v in r.items() if k != "samples_s"}
                        for r in rows]}
    lines = ["%8s %10s %9s %9s %9s" % ("height", "bytes", "MB/s", "fps",
                                       "ms/frame")]
    for r in rows:
        lines.append("%8d %10d %9.3f %9.2f %9.2f"
                     % (r["height"], r["payload_bytes"], r["mb_per_s"],
                        r["fps"], r["median_s"] * 1e3))
    lines.append("")
    lines.append("full-speed USB theoretical ceiling: %.3f MB/s"
                 % measure.USB_FS_THEORETICAL_MBPS)
    _print(args, lines, payload)
    return EXIT_OK


def cmd_selftest(args):
    """Run pud_usb's offline encoder self-test (no device involved)."""
    import io
    import contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        pud_usb._selftest()
    text = buf.getvalue()
    payload = {"ok": True, "output": text}
    _print(args, text.rstrip().splitlines(), payload)
    return EXIT_OK


def read_constant(path, name):
    """Value of one `#define NAME <literal>` in a C header/source file.

    Only the simple forms are supported (`0x2e8a`, `65536`, `(32 * 1024)`) --
    the point is to let a test read the firmware's own constant instead of
    restating it, so that a mirror drifting from the source is caught by a
    test rather than by a reader.  Returns an int, or None.

    **It does not evaluate the preprocessor**: for a constant defined once per
    platform (`#if defined(PICO_RP2040) ... #else ...`) it returns the first
    textual definition, so a caller that cares must check for both branches
    (see tests/test_protocol_constants.py).
    """
    import re

    text = open(path, "r", encoding="utf-8", errors="replace").read()
    pattern = (r"^[ \t]*#[ \t]*define[ \t]+" + re.escape(name) + r"[ \t]+(.+)$")
    for line in text.splitlines():
        m = re.match(pattern, line)
        if not m:
            continue
        expr = m.group(1)
        # drop a trailing comment in either style, then integer suffixes
        expr = expr.split("/*")[0].split("//")[0].strip()
        expr = re.sub(r"(?<=[0-9a-fA-F])[uUlL]+\s*$", "", expr).strip()
        if not re.fullmatch(r"[0-9a-fA-FxX()\s*]+", expr):
            continue
        factors = re.findall(r"0[xX][0-9a-fA-F]+|\d+", expr)
        if not factors:
            continue
        value = 1
        for factor in factors:
            value *= int(factor, 0)
        return value
    return None


def cmd_src(args):
    """Read constants straight out of the firmware source.

    This is the drift check's building block: a test can assert that a mirror
    in `tools/pud_usb.py` equals what `include/pud.h` actually says, instead of
    hard-coding the number a second time (see tests/test_protocol_constants.py).
    """
    found, missing = {}, []
    for spec in args.constant:
        if "=" not in spec:
            print("--constant wants FILE:NAME, got %r" % spec, file=sys.stderr)
            return EXIT_INVALID_USAGE
        path, name = spec.split("=", 1)
        if not os.path.exists(path):
            missing.append("%s (no such file)" % path)
            continue
        value = read_constant(path, name)
        if value is None:
            missing.append("%s:%s" % (path, name))
        else:
            found["%s:%s" % (path, name)] = value
    payload = {"constants": found, "missing": missing}
    _print(args, ["%s = %d (0x%x)" % (k, v, v) for k, v in found.items()]
           + ["missing: %s" % m for m in missing], payload)
    if missing:
        print("not found: %s" % ", ".join(missing), file=sys.stderr)
        return EXIT_FAIL
    return EXIT_OK


def build_parser():
    ap = argparse.ArgumentParser(
        prog="pudctl", description=__doc__.strip().splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", action="version",
                    version="%(prog)s " + VERSION)
    sub = ap.add_subparsers(dest="command", metavar="<subcommand>")

    def common(p):
        p.add_argument("--json", action="store_true")
        p.add_argument("--quiet", action="store_true")
        p.add_argument("--verbose", action="store_true")
        p.add_argument("--timeout", type=float, default=120.0,
                       help="ignored by this tool; kept for CLI uniformity")
        p.add_argument("--device-timeout", type=float, default=None,
                       help="per-transfer timeout in ms")
        p.add_argument("--vendor", type=lambda v: int(v, 0), default=pud_usb.VID)
        p.add_argument("--product", type=lambda v: int(v, 0), default=pud_usb.PID)
        return p

    common(sub.add_parser("probe", help="is the panel there?")).set_defaults(
        func=cmd_probe)
    common(sub.add_parser("caps", help="PUD_CMD_GET_CAPS report")).set_defaults(
        func=cmd_caps)
    p = common(sub.add_parser("params", help="read/write runtime parameters"))
    p.add_argument("--set", action="append", metavar="FIELD=VALUE",
                   help="brightness=NN or rotation=N (repeatable)")
    p.set_defaults(func=cmd_params)
    common(sub.add_parser("sn", help="board unique id")).set_defaults(
        func=cmd_sn)
    p = common(sub.add_parser("touch", help="read EP4 touch reports"))
    p.add_argument("--mode", choices=("push", "poll"), default="push")
    p.add_argument("--seconds", type=float, default=5.0)
    p.add_argument("--idle-timeout", type=float, default=200.0,
                   help="poll mode: ms to wait after each request")
    p.set_defaults(func=cmd_touch)
    p = common(sub.add_parser("meter", help="EP1 throughput sweep"))
    p.add_argument("--xres", type=int, default=480)
    p.add_argument("--yres", type=int, default=320)
    p.add_argument("--codec", default="qoi", choices=sorted(pud_usb.ENCODERS))
    p.add_argument("--heights", type=int, nargs="+",
                   default=[8, 16, 32, 64, 128, 320])
    p.add_argument("--repeats", type=int, default=20)
    p.add_argument("--gap-ms", type=float, default=0.0)
    p.set_defaults(func=cmd_meter)
    common(sub.add_parser("selftest",
                          help="offline encoder self-test")).set_defaults(
        func=cmd_selftest)
    p = common(sub.add_parser("src",
                              help="read #define constants from firmware source"))
    p.add_argument("--constant", action="append", metavar="FILE=NAME",
                   required=True, help="e.g. include/pud.h=PUD_PROTO_VER")
    p.set_defaults(func=cmd_src)
    return ap


def main(argv=None):
    ap = build_parser()
    args = ap.parse_args(argv)
    if not getattr(args, "command", None):
        ap.print_help()
        return EXIT_INVALID_USAGE
    try:
        return args.func(args)
    except pud_usb.PudError as exc:
        print("ENVIRONMENT_ERROR: %s" % exc, file=sys.stderr)
        return EXIT_ENVIRONMENT_ERROR
    except OSError as exc:
        # usb.core.USBError is an OSError, so a transport failure lands here.
        # The workspace convention reserves exit 1 for "the device answered and
        # the answer was wrong"; "the hardware or the permissions are not
        # there" is ENVIRONMENT_ERROR.  A traceback with exit 1 reads like the
        # panel failed something it was never asked.
        print("ENVIRONMENT_ERROR: %s" % exc, file=sys.stderr)
        return EXIT_ENVIRONMENT_ERROR
    except KeyboardInterrupt:
        return EXIT_ENVIRONMENT_ERROR


if __name__ == "__main__":
    sys.exit(main())
