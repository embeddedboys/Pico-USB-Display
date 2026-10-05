#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
Full-screen **JPEG** bench (host side): send one self-contained JPEG of the whole
panel rectangle, N times, and report the end-to-end period.

为什么单独有这个工具：JPEG 的协议语义是"图自带尺寸、主机裁好"，所以主机只要把字节递过去
（`Display.send_raw`），可是仓库里一直没有发 JPEG 的脚本（见 `notes/todo.md` 的未结案项）。
一次发整屏、`xs=ys=0`：**JPEG 两条路都只用于整屏**，局刷在 JPEGDEC 上会卡死显示
（见 `notes/decoder-architecture.md`）。

What is measured
----------------
计时只含 EP1 控制请求 + 批量传输（`send_raw`），**JPEG 编码在计时循环之外**先做好，
所以数字描述的是链路 + 设备，不含 Pillow。设备侧开 `DECODER_STATS=1` 时另有
`g_qoi_stat_draw_us`（解码 + 刷屏）等计数器，用 gdb 读取：

    printf "%u %u %u %u %u\\n", g_qoi_stat_draw_us, g_qoi_stat_flush_us,
           g_qoi_stat_pixels, g_qoi_stat_calls, g_qoi_stat_frames

Reported:
  min       最快一帧（设备空闲）≈ 纯 USB 传输
  median    稳态周期；median/min 大 ⇒ 设备侧（解码）受限，接近 1 ⇒ USB 受限
  fps / MB/s / B/帧

Usage:
    ./tools/jpeg_bench.py --image assets/xfce.jpg --quality 85 --frames 60
    ./tools/jpeg_bench.py --jpeg /tmp/frame.jpg --frames 60 --json
    ./tools/jpeg_bench.py --image assets/xfce.jpg --dry-run        # 不接设备

设备不能被 pud 内核驱动占用；免 root 的 udev 规则见 pud_usb.py。
'''

import argparse
import io
import json
import os
import statistics
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import pud_usb  # noqa: E402

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_INVALID_USAGE = 2
EXIT_ENVIRONMENT_ERROR = 3


def encode_from_image(path, quality, width, height):
    """图片 → 面板几何的 JPEG（4:2:0）。4:2:0 是实测更快的那一档（见 notes）。"""
    try:
        from PIL import Image
    except ImportError as exc:                       # pragma: no cover
        raise RuntimeError("需要 Pillow：pip install pillow") from exc
    if not os.path.exists(path):
        raise FileNotFoundError(path)
    img = Image.open(path).convert("RGB").resize((width, height), Image.LANCZOS)
    buf = io.BytesIO()
    # subsampling=2 = 4:2:0（JPEGDEC 实测 66.1 ms vs 4:4:4 的 76.4 ms）
    img.save(buf, "JPEG", quality=quality, subsampling=2, optimize=False)
    return buf.getvalue()


def load_jpeg(args, width, height):
    if args.jpeg:
        if not os.path.exists(args.jpeg):
            raise FileNotFoundError(args.jpeg)
        return open(args.jpeg, "rb").read()
    return encode_from_image(args.image, args.quality, width, height)


def measure(disp, payload, frames, gap_ms):
    """发 frames 次整屏 JPEG；返回每帧周期（秒）。编码已在循环外做完。"""
    xres, yres = disp.width, disp.height
    periods = []
    for _ in range(frames):
        t0 = time.perf_counter()
        disp.send_raw(payload, 0, 0, xres - 1, yres - 1)
        periods.append(time.perf_counter() - t0)
        if gap_ms:
            time.sleep(gap_ms / 1000.0)
    return periods


def report(args, payload, periods, width, height):
    total = sum(periods)
    ordered = sorted(periods)
    fast = ordered[0] * 1e3
    med = statistics.median(periods) * 1e3
    p90 = ordered[max(int(len(ordered) * 0.9) - 1, 0)] * 1e3
    ratio = med / fast if fast > 0 else 0.0
    verdict = ("USB 受限 (steady/min = %.2fx)" % ratio if ratio < 1.3
               else "设备侧受限 (steady/min = %.2fx)" % ratio)
    result = {
        "jpeg_bytes": len(payload),
        "geometry": "%dx%d" % (width, height),
        "frames": len(periods),
        "min_ms": round(fast, 3),
        "median_ms": round(med, 3),
        "p90_ms": round(p90, 3),
        "fps": round(len(periods) / total, 2),
        "mbyte_per_s": round(len(payload) * len(periods) / total / 1e6, 3),
        "steady_over_min": round(ratio, 2),
    }
    if args.json:
        print(json.dumps({"tool": "jpeg_bench", "observation": result,
                          "note": "设备侧 draw_us 需另用 gdb 读 DECODER_STATS 计数器"},
                         ensure_ascii=False))
        return result
    print("=== 全屏 JPEG ===")
    print("  几何                    %dx%d" % (width, height))
    print("  JPEG 字节               %d B" % len(payload))
    print("  min(空闲态, ≈纯USB)     %.2f ms" % fast)
    print("  稳态                    %.2f ms  (p90 %.2f)"
          % (med, p90))
    print("  帧率                    %.2f fps" % (len(periods) / total))
    print("  压缩吞吐                %.3f MB/s" % (len(payload) * len(periods)
                                                 / total / 1e6))
    print("  判定                    %s" % verdict)
    print()
    return result


def main():
    ap = argparse.ArgumentParser(description="Full-screen JPEG bench")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--jpeg", help="already-encoded JPEG to send as-is")
    src.add_argument("--image", help="image to encode to panel geometry (needs Pillow)")
    ap.add_argument("--quality", type=int, default=85, help="JPEG quality 1..100")
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--gap-ms", type=float, default=0.0,
                    help="sleep between frames (0 = as fast as the device takes them)")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--dry-run", action="store_true", help="encode and report, no device")
    args = ap.parse_args()

    if args.dry_run:
        # 面板几何拿不到就按 480x320 算（本板旋转 1）
        payload = load_jpeg(args, 480, 320)
        print("=== 全屏 JPEG（dry-run）===")
        print("  JPEG 字节               %d B" % len(payload))
        print("  单次传输上限             %d B" % pud_usb.USB_TRANS_MAX_SIZE)
        return (EXIT_OK if len(payload) <= pud_usb.USB_TRANS_MAX_SIZE
                else EXIT_FAIL)

    try:
        with pud_usb.open_device() as disp:
            width, height = disp.width, disp.height
            payload = load_jpeg(args, width, height)
            # 公开字段算上限（send_raw 内部也会依 _payload_limit 再判一次）
            limit = (min(pud_usb.USB_TRANS_MAX_SIZE, disp.frame_max)
                     - pud_usb.EP1_HEADER_SIZE)
            if len(payload) > limit:
                print("INVALID_USAGE: %d B 的 JPEG 超过本设备单次传输上限 %d B"
                      % (len(payload), limit), file=sys.stderr)
                return EXIT_INVALID_USAGE
            periods = measure(disp, payload, args.frames, args.gap_ms)
    except pud_usb.PudError as exc:
        print("ENVIRONMENT_ERROR: %s" % exc, file=sys.stderr)
        return EXIT_ENVIRONMENT_ERROR
    except (RuntimeError, FileNotFoundError) as exc:
        print("ENVIRONMENT_ERROR: %s" % exc, file=sys.stderr)
        return EXIT_ENVIRONMENT_ERROR

    report(args, payload, periods, width, height)
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
