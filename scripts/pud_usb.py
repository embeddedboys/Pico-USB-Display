#!/usr/bin/env python3

#
# Copyright (c) 2026 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
Shared helpers for driving the Pico USB Display from userspace.

Dependencies
------------
Required:
    pyusb           the only hard dependency
Optional, in order of preference:
    Pillow          image decoding -- tiny (~3 MB) and enough for JPEG/PNG/BMP
    ffmpeg (CLI)    video decoding, via a rawvideo pipe (no python binding)
    opencv-python   fallback image decoder if Pillow is unavailable
    numpy           only used to accelerate RGB565 packing; not required

Pillow is recommended over opencv-python for still images: it is roughly
twenty times smaller and decodes the same formats. Nothing here needs numpy --
without it the RGB565 conversion falls back to a plain python loop that still
handles a 480x320 frame in a few tens of milliseconds.

This module also carries the RGB565 encoders for every codec the device can
be built with: QOI and RLE are byte-for-byte identical to the C libraries the
firmware and the kernel driver share (rgb565_qoi.c, rgb565_rle.c), and LZ4 is
liblz4 itself -- the implementation the kernel links -- via `lz4.block`.  Run
`python3 pud_usb.py` to check the two hand written ones against a reference
vector.

The device must not be bound to the pud kernel driver, since pyusb has to
claim the interface. Install the bundled udev rule once to avoid needing root:

    sudo cp 60-pico-usb-display.rules /etc/udev/rules.d/
    sudo udevadm control --reload-rules && sudo udevadm trigger
'''

import os
import struct
import subprocess
import sys
import time

# ---------------------------------------------------------------------------
# Protocol (keep in sync with src/cherryusb/usbd_vendor.h)
# ---------------------------------------------------------------------------

VID = 0x2E8A
PID = 0x0001

EP_DIR_OUT = 0x00
EP_DIR_IN = 0x80
TYPE_VENDOR = 0x40

EP1_OUT_ADDR = EP_DIR_OUT | 0x01
EP2_IN_ADDR = EP_DIR_IN | 0x02
EP4_IN_ADDR = EP_DIR_IN | 0x04

REQ_EP0_OUT = 0x00
REQ_EP0_IN = 0x01
REQ_EP1_OUT = 0x02
REQ_EP2_IN = 0x03
REQ_EP4_IN = 0x05

CMD_GET_SN = 0x01
CMD_GET_CAPS = 0x02

#: Device capability report (``PUD_CMD_GET_CAPS``): magic, protocol version,
#: the largest single EP1 transfer the device accepts, and its active decoder.
#: Kept in sync with ``struct pud_caps`` in the firmware and the driver.
CAPS_MAGIC = 0x43445550  # "PUDC"
CAPS_STRUCT = struct.Struct("<IIII")

#: Largest payload the firmware accepts in one transfer (its frame slot is
#: 64 KiB, and the protocol carries the length in a 16-bit field).
#: This is the *host* ceiling; the device reports its own through
#: ``Display.frame_max`` and the smaller of the two wins.
USB_TRANS_MAX_SIZE = 65535

#: A transfer is decoded as one self-contained QOI image, so a rectangle is
#: split into horizontal bands that survive QOI's worst case of 3 bytes per
#: pixel plus the 8-byte header and 8-byte end marker. Same rule the driver
#: uses (PUD_MAX_BAND_PIXELS), which costs nothing in sustained throughput.
PUD_MAX_BAND_PIXELS = (USB_TRANS_MAX_SIZE - 16) // 3

DEFAULT_TIMEOUT_MS = 5000

#: EP4 touch report, 8 bytes, byte-explicit (see notes/usb-protocol.md):
#: flags, x >> 8, x & 0xff, y >> 8, y & 0xff, sequence, version, reserved.
#: The device pushes one per poll while the panel is held plus one on release;
#: `Display.touch_request()` is the polling alternative.
TOUCH_REPORT_SIZE = 8
TOUCH_VERSION = 1
TOUCH_PRESSED = 0x01


def parse_touch_report(buf):
    """Decode one EP4 report into a dict."""
    if len(buf) < TOUCH_REPORT_SIZE:
        raise PudError("short touch report: %d of %d bytes"
                       % (len(buf), TOUCH_REPORT_SIZE))

    return {
        "pressed": bool(buf[0] & TOUCH_PRESSED),
        "flags": buf[0],
        "x": (buf[1] << 8) | buf[2],
        "y": (buf[3] << 8) | buf[4],
        "seq": buf[5],
        "version": buf[6],
        "reserved": buf[7],
    }


class PudError(Exception):
    """Anything that should reach the user as a plain message."""


# ---------------------------------------------------------------------------
# RGB565 QOI encoder
#
# Format: magic "q565" + uint32 pixel_count (LE) + chunks + 8-byte end marker.
# See rgb565_qoi.h for the full chunk layout; this mirrors the C encoder.
# ---------------------------------------------------------------------------

_QOI_MAGIC = b"q565"
_QOI_OP_INDEX = 0x00
_QOI_OP_DIFF = 0x40
_QOI_OP_LUMA = 0x80
_QOI_OP_RUN = 0xC0
_QOI_OP_RGB565 = 0xFE
_QOI_PADDING = bytes([0, 0, 0, 0, 0, 0, 0, 1])


def _qoi_hash(px):
    return ((((px >> 11) & 0x1F) * 3 + ((px >> 5) & 0x3F) * 5 + (px & 0x1F) * 7)
            & 0x3F)


#: Run-length limits of the RGB565 RLE format (src/decoders/rle/).
RLE_MAX_RUN = 128


def rle_max_compressed_size(pixel_count):
    """Worst case for `pixel_count` pixels: 4 byte header + 3 bytes each."""
    return 0 if pixel_count <= 0 else 4 + 3 * pixel_count


def rle_encode(pixels):
    """Compress RGB565 pixels (bytes or an iterable of ints) into an RLE stream.

    Byte-identical to the C library's rgb565_rle_compress(): a 4 byte
    little-endian pixel count, then runs.  A control byte with bit 7 set is a
    repeat run (one pixel, repeated `(ctl & 0x7f) + 1` times); otherwise it is a
    literal run of that many distinct pixels.  Every pixel is two bytes,
    little-endian.
    """
    if isinstance(pixels, (bytes, bytearray, memoryview)):
        view = memoryview(pixels)
        if view.itemsize != 2 or len(pixels) % 2:
            view = memoryview(bytes(pixels)).cast("H")
        else:
            view = view.cast("H")
    else:
        view = pixels

    n = len(view)
    out = bytearray()
    out += struct.pack("<I", n)

    pos = 0
    while pos < n:
        limit = min(RLE_MAX_RUN, n - pos)
        first = view[pos]
        run = 1
        while run < limit and view[pos + run] == first:
            run += 1

        if run >= 2:
            out.append(0x80 | (run - 1))
            out += struct.pack("<H", first)
            pos += run
            continue

        # literal run: stop where a beneficial repeat run would start, i.e.
        # where the next two pixels are equal (same rule as the C encoder)
        lit = 1
        while lit < limit:
            nxt = pos + lit
            if nxt + 1 < n and view[nxt] == view[nxt + 1]:
                break
            lit += 1
        out.append(lit - 1)
        out += struct.pack("<%dH" % lit, *view[pos:pos + lit])
        pos += lit

    return bytes(out)


def lz4_encode(pixels):
    """Compress RGB565 pixels (bytes or an iterable of ints) into an LZ4 block.

    Unlike QOI and RLE there is no hand written encoder here: `lz4.block` is
    the reference liblz4, i.e. the very implementation the kernel links, so a
    stream built here is what the driver's LZ4_compress_default() produces.

    `store_size=False` keeps it a bare LZ4 block -- the decompressed length is
    implied by the transfer's window, not stored in the stream.  The device
    decodes one *band* per transfer (an LZ4 block cannot be decoded in pieces),
    so callers band first; `Display.send_rgb565()` does that for you.
    """
    try:
        import lz4.block
    except ImportError:
        raise PudError("the lz4 package is required: pip install lz4")

    if not isinstance(pixels, (bytes, bytearray, memoryview)):
        pixels = struct.pack("<%dH" % len(pixels), *pixels)

    return lz4.block.compress(bytes(pixels), store_size=False)


def qoi_encode(pixels):
    """Compress RGB565 pixels (bytes or an iterable of ints) into a QOI stream.

    `bytes` input is the fast path: the buffer is viewed as native uint16 so
    no per-pixel python object has to be built.
    """
    if isinstance(pixels, (bytes, bytearray, memoryview)):
        view = memoryview(pixels)
        if view.itemsize != 2 or len(pixels) % 2:
            view = memoryview(bytes(pixels)).cast("H")
        else:
            view = view.cast("H")
    else:
        view = pixels

    out = bytearray()
    out += _QOI_MAGIC
    out += struct.pack("<I", len(view))

    index = [0] * 64
    prev = 0
    run = 0
    last = len(view) - 1
    append = out.append

    for i, px in enumerate(view):
        if px == prev:
            run += 1
            if run == 62 or i == last:
                append(_QOI_OP_RUN | (run - 1))
                run = 0
            continue

        if run:
            append(_QOI_OP_RUN | (run - 1))
            run = 0

        slot = _qoi_hash(px)
        if index[slot] == px:
            append(_QOI_OP_INDEX | slot)
        else:
            index[slot] = px

            r = (px >> 11) & 0x1F
            g = (px >> 5) & 0x3F
            b = px & 0x1F
            vr = (r - ((prev >> 11) & 0x1F)) & 0x1F
            vg = (g - ((prev >> 5) & 0x3F)) & 0x3F
            vb = (b - (prev & 0x1F)) & 0x1F
            if vr > 15:
                vr -= 32
            if vg > 31:
                vg -= 64
            if vb > 15:
                vb -= 32

            if -3 < vr < 2 and -3 < vg < 2 and -3 < vb < 2:
                append(_QOI_OP_DIFF | ((vr + 2) << 4) | ((vg + 2) << 2)
                       | (vb + 2))
            else:
                vgr = vr - vg
                vgb = vb - vg
                if -9 < vgr < 8 and -33 < vg < 32 and -9 < vgb < 8:
                    append(_QOI_OP_LUMA | (vg + 32))
                    append(((vgr + 8) << 4) | (vgb + 8))
                else:
                    append(_QOI_OP_RGB565)
                    out += struct.pack("<H", px)

        prev = px

    out += _QOI_PADDING
    return bytes(out)


#: The encoders a host may send with.  The device has to be built for the same
#: one (DECODER_TYPE); `Display.query_caps()` asks which.
ENCODERS = {
    "qoi": qoi_encode,
    "rle": rle_encode,
    "lz4": lz4_encode,
}


# ---------------------------------------------------------------------------
# Pixels
# ---------------------------------------------------------------------------

def rgb888_to_rgb565(buf, width, height, swap=False):
    """Pack interleaved RGB888 bytes into little-endian RGB565 bytes."""
    try:
        import numpy as np
    except ImportError:
        np = None

    if np is not None:
        a = np.frombuffer(buf, dtype=np.uint8, count=width * height * 3)
        a = a.reshape(height, width, 3)
        r = a[:, :, 0].astype(np.uint16)
        g = a[:, :, 1].astype(np.uint16)
        b = a[:, :, 2].astype(np.uint16)
        if swap:
            r, b = b, r
        return (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)) \
            .astype("<u2").tobytes()

    out = bytearray(width * height * 2)
    j = 0
    for i in range(0, width * height * 3, 3):
        r = buf[i]
        g = buf[i + 1]
        b = buf[i + 2]
        if swap:
            r, b = b, r
        px = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[j] = px & 0xFF
        out[j + 1] = (px >> 8) & 0xFF
        j += 2
    return bytes(out)


def crop_rgb565(buf, width, x, y, w, h):
    """Cut a packed RGB565 rectangle out of a wider buffer."""
    stride = width * 2
    out = bytearray(w * h * 2)
    j = 0
    for row in range(y, y + h):
        start = row * stride + x * 2
        out[j:j + w * 2] = buf[start:start + w * 2]
        j += w * 2
    return bytes(out)


def _letterbox(raw, src_w, src_h, width, height):
    """Center-fit RGB888 bytes onto a black canvas of the target size."""
    if src_w == width and src_h == height:
        return raw
    scale = min(width / src_w, height / src_h)
    dst_w = max(1, min(width, int(src_w * scale)))
    dst_h = max(1, min(height, int(src_h * scale)))
    x0 = (width - dst_w) // 2
    y0 = (height - dst_h) // 2

    out = bytearray(b"\x00" * (width * height * 3))
    for y in range(dst_h):
        sy = y * src_h // dst_h
        srow = sy * src_w * 3
        drow = ((y0 + y) * width + x0) * 3
        for x in range(dst_w):
            sx = x * src_w // dst_w
            s = srow + sx * 3
            out[drow + x * 3:drow + x * 3 + 3] = raw[s:s + 3]
    return bytes(out)


# ---------------------------------------------------------------------------
# Images and video
# ---------------------------------------------------------------------------

def _load_image_pillow(path, width, height, fit):
    from PIL import Image

    img = Image.open(path).convert("RGB")
    if fit:
        img.thumbnail((width, height), Image.LANCZOS)
        canvas = Image.new("RGB", (width, height), (0, 0, 0))
        canvas.paste(img, ((width - img.width) // 2, (height - img.height) // 2))
        img = canvas
    else:
        img = img.resize((width, height), Image.LANCZOS)
    return img.tobytes()


def _load_image_cv2(path, width, height, fit):
    import cv2
    import numpy as np

    img = cv2.imread(path)                       # BGR
    if img is None:
        raise PudError("cannot decode image: %s" % path)
    h, w = img.shape[:2]
    if fit:
        scale = min(width / w, height / h)
        nw = max(1, min(width, int(w * scale)))
        nh = max(1, min(height, int(h * scale)))
        resized = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_AREA)
        canvas = np.zeros((height, width, 3), np.uint8)
        y0, x0 = (height - nh) // 2, (width - nw) // 2
        canvas[y0:y0 + nh, x0:x0 + nw] = resized
        img = canvas
    else:
        img = cv2.resize(img, (width, height), interpolation=cv2.INTER_AREA)
    return img[:, :, ::-1].tobytes()             # BGR -> RGB


def load_image(path, width, height, fit=True):
    """Decode `path` and return RGB888 bytes sized `width` x `height`."""
    if not os.path.isfile(path):
        raise PudError("no such file: %s" % path)
    try:
        return _load_image_pillow(path, width, height, fit)
    except ImportError:
        pass
    try:
        return _load_image_cv2(path, width, height, fit)
    except ImportError:
        raise PudError("no image decoder available: pip install pillow "
                       "(or opencv-python)")


def video_frames(path, width, height, fps=None, fit=True):
    """Yield RGB888 frames of `path` as `width` x `height`.

    Uses the ffmpeg CLI and a rawvideo pipe, so no python video binding is
    needed and nothing is written to disk.
    """
    if not os.path.isfile(path):
        raise PudError("no such file: %s" % path)

    filters = []
    if fit:
        filters.append("scale=%d:%d:force_original_aspect_ratio=decrease"
                       % (width, height))
        filters.append("pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=black"
                       % (width, height))
    else:
        filters.append("scale=%d:%d" % (width, height))
    if fps:
        filters.append("fps=%s" % fps)

    cmd = ["ffmpeg", "-v", "error", "-i", path,
           "-vf", ",".join(filters),
           "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]

    return ffmpeg_frames(cmd, width, height)


def ffmpeg_frames(cmd, width, height):
    """Yield RGB888 frames of `width` x `height` from an ffmpeg invocation.

    `cmd` must write rawvideo rgb24 to stdout; anything else (a screen grab,
    a filter chain, ...) is up to the caller.
    """
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
    except FileNotFoundError:
        raise PudError("ffmpeg not found -- install it")

    frame_bytes = width * height * 3
    yielded = 0
    try:
        while True:
            buf = proc.stdout.read(frame_bytes)
            if len(buf) < frame_bytes:
                break
            yielded += 1
            yield buf
    finally:
        # The caller may stop early (a frame limit, Ctrl-C), which closes the
        # pipe under ffmpeg. Shut the producer down deliberately and stay
        # quiet about it: ffmpeg logs "Broken pipe" while draining its trailer
        # and exits non-zero even though nothing actually went wrong.
        proc.stdout.close()
        stopped_early = proc.poll() is None
        if stopped_early:
            proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        err = proc.stderr.read().decode(errors="replace").strip()
        proc.stderr.close()
        if err and not stopped_early and proc.returncode not in (0, -13, -15):
            if not yielded:
                raise PudError("ffmpeg produced no frames: %s" % err)
            sys.stderr.write("ffmpeg: %s\n" % err)


# ---------------------------------------------------------------------------
# Device
# ---------------------------------------------------------------------------

def open_device():
    """Find the display and claim its interface."""
    try:
        import usb.core
        import usb.util
    except ImportError:
        raise PudError("pyusb is required: pip install pyusb")

    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        raise PudError("device %04x:%04x not found -- is it plugged in?"
                       % (VID, PID))

    try:
        usb.util.claim_interface(dev, 0)
    except usb.core.USBError as exc:
        err = getattr(exc, "errno", None)
        if err == 13:
            raise PudError(
                "no permission to open the device (Access denied).\n"
                "Install the udev rule shipped in this repo, then replug:\n"
                "  sudo cp 60-pico-usb-display.rules /etc/udev/rules.d/\n"
                "  sudo udevadm control --reload-rules && sudo udevadm trigger")
        if err == 16:
            raise PudError(
                "the interface is busy: the pud kernel driver is still bound.\n"
                "  sudo rmmod pud\n"
                "If rmmod reports the module is in use, a compositor still\n"
                "holds the DRM card -- reboot without loading the driver.")
        raise PudError("cannot claim the interface: %s" % exc)

    disp = Display(dev)
    # Not fatal: an older firmware simply keeps the host-side defaults.
    try:
        disp.query_caps()
    except Exception:
        pass
    return disp


class Display:
    """A claimed device; use as a context manager."""

    def __init__(self, dev, width=480, height=320):
        import usb.core
        import usb.util

        self.dev = dev
        self.usb = usb.core
        self.util = usb.util
        self.width = width
        self.height = height

        # Device-reported limits; defaults are the host-side ceiling and are
        # replaced by query_caps().  An RP2040 firmware (half the SRAM) accepts
        # half-size transfers, so anything that bands a rectangle must use
        # band_pixels rather than the module constant.
        self.caps = None
        self.frame_max = USB_TRANS_MAX_SIZE
        self.band_pixels = PUD_MAX_BAND_PIXELS
        self.decoder_type = None

    # -- lifecycle --------------------------------------------------------
    def close(self):
        if self.dev is not None:
            try:
                self.util.release_interface(self.dev, 0)
            except Exception:
                pass
            self.dev = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # -- protocol ---------------------------------------------------------
    def _window(self, xs, ys, xe, ye, size):
        self.dev.ctrl_transfer(
            TYPE_VENDOR | EP_DIR_OUT, REQ_EP1_OUT, 0, 0,
            struct.pack("<HHHHI", xs, ys, xe, ye, size))

    def _check_rect(self, xs, ys, xe, ye):
        if xs < 0 or ys < 0 or xe >= self.width or ye >= self.height:
            raise PudError(
                "rectangle (%d,%d)-(%d,%d) is outside the %dx%d panel"
                % (xs, ys, xe, ye, self.width, self.height))

    def send_raw(self, payload, xs, ys, xe, ye, timeout=None):
        """Send an already-compressed payload for one rectangle.

        Returns (payload_bytes, seconds). Used for codecs whose framing is
        not QOI, and by anything that wants full control over the stream.
        """
        timeout = timeout or DEFAULT_TIMEOUT_MS
        limit = min(USB_TRANS_MAX_SIZE, self.frame_max)
        if len(payload) > limit:
            raise PudError(
                "%d byte payload exceeds the %d byte limit this device accepts"
                % (len(payload), limit))
        self._check_rect(xs, ys, xe, ye)
        self._window(xs, ys, xe, ye, len(payload))
        t0 = time.perf_counter()
        self.dev.write(EP1_OUT_ADDR, payload, timeout=timeout)
        return len(payload), time.perf_counter() - t0

    def send_rgb565(self, rgb565, width, height, xs=0, ys=0, timeout=None,
                    codec="qoi"):
        """Send a rectangle of RGB565 pixels, banding it if needed.

        The rectangle is split into horizontal bands of at most `band_pixels`
        pixels (what the device reports through PUD_CMD_GET_CAPS), and each band
        goes as one self-contained stream -- which is what the device's decoder
        expects, for every codec:

          qoi / rle   the band is one QOI/RLE image
          lz4         the band is one LZ4 block; an LZ4 block cannot be decoded
                      in pieces, so the device holds exactly one band at a time

        Returns (bands, payload_bytes, seconds).
        """
        try:
            encode = ENCODERS[codec]
        except KeyError:
            raise PudError("unknown codec %r (have: %s)"
                           % (codec, ", ".join(sorted(ENCODERS))))

        timeout = timeout or DEFAULT_TIMEOUT_MS
        self._check_rect(xs, ys, xs + width - 1, ys + height - 1)
        rows = max(1, self.band_pixels // width)
        limit = min(USB_TRANS_MAX_SIZE, self.frame_max)
        total = 0
        bands = 0
        t0 = time.perf_counter()

        for y in range(0, height, rows):
            bh = min(rows, height - y)
            start = y * width * 2
            band = rgb565[start:start + bh * width * 2]
            payload = encode(band)
            if len(payload) > limit:
                raise PudError("band of %d bytes exceeds the %d byte limit"
                               % (len(payload), limit))
            self._window(xs, ys + y, xs + width - 1, ys + y + bh - 1,
                         len(payload))
            self.dev.write(EP1_OUT_ADDR, payload, timeout=timeout)
            total += len(payload)
            bands += 1

        return bands, total, time.perf_counter() - t0

    def send_full(self, rgb565, **kw):
        return self.send_rgb565(rgb565, self.width, self.height, **kw)

    def touch_request(self, timeout=None):
        """Ask the device to arm one touch report (the polling model).

        The firmware also pushes reports on its own, so a host that just keeps
        reading EP4 does not need this; it exists for the driver version that
        requested one report per sample.
        """
        self.dev.ctrl_transfer(TYPE_VENDOR | EP_DIR_OUT, REQ_EP4_IN, 0, 0, None,
                               timeout=int(timeout or DEFAULT_TIMEOUT_MS))

    def read_touch(self, timeout=None):
        """Read one EP4 report, blocking until the device sends one.

        Raises ``usb.core.USBTimeoutError`` when nothing arrives, which is the
        normal state while the panel is untouched.
        """
        # pyusb wants an integer number of milliseconds here, not a float
        buf = self.dev.read(EP4_IN_ADDR, TOUCH_REPORT_SIZE,
                            timeout=int(timeout or DEFAULT_TIMEOUT_MS))
        return parse_touch_report(bytes(buf))

    def get_sn(self):
        """Read the 8-byte board unique id."""
        self.dev.ctrl_transfer(
            TYPE_VENDOR | EP_DIR_OUT, REQ_EP2_IN, 0, 0,
            struct.pack("<HH", CMD_GET_SN, 8))
        return bytes(self.dev.read(EP2_IN_ADDR, 8, timeout=DEFAULT_TIMEOUT_MS))

    def query_caps(self, timeout=None):
        """Ask the device what it accepts (``PUD_CMD_GET_CAPS``).

        Updates ``frame_max``, ``band_pixels`` and ``decoder_type``.  A device
        without the command answers with whatever was left in its buffer, so
        the magic (not the transfer length) is what decides; on any mismatch the
        conservative defaults are kept and None is returned.
        """
        timeout = timeout or DEFAULT_TIMEOUT_MS
        self.dev.ctrl_transfer(
            TYPE_VENDOR | EP_DIR_OUT, REQ_EP2_IN, 0, 0,
            struct.pack("<HH", CMD_GET_CAPS, CAPS_STRUCT.size))
        raw = bytes(self.dev.read(EP2_IN_ADDR, CAPS_STRUCT.size,
                                  timeout=timeout))
        if len(raw) < CAPS_STRUCT.size:
            return None
        magic, proto_ver, frame_max, decoder_type = CAPS_STRUCT.unpack(raw)
        if magic != CAPS_MAGIC or not (0 < frame_max <= (1 << 20)):
            return None

        self.caps = dict(proto_ver=proto_ver, frame_max=frame_max,
                         decoder_type=decoder_type)
        self.frame_max = min(USB_TRANS_MAX_SIZE, frame_max)
        if self.frame_max > 16:
            self.band_pixels = max(1, (self.frame_max - 16) // 3)
        self.decoder_type = decoder_type
        return self.caps


# ---------------------------------------------------------------------------
# Self test
# ---------------------------------------------------------------------------

_REFERENCE_PIXELS = (0x1234, 0x1235, 0x1236, 0x1236, 0x1236, 0xF800, 0x07E0,
                     0x1234)
_RLE_REFERENCE_STREAM = bytes.fromhex(
    "0800000001341235128236120200f8e0073412")
_REFERENCE_STREAM = bytes.fromhex(
    "7135363508000000fe34126b6bc1fe00f876270000000000000001")


def _selftest():
    got = qoi_encode(_REFERENCE_PIXELS)
    if got != _REFERENCE_STREAM:
        raise PudError("QOI encoder mismatch:\n  got %s\n  want %s"
                       % (got.hex(), _REFERENCE_STREAM.hex()))
    # the bytes path must produce the same stream as the tuple path
    packed = struct.pack("<8H", *_REFERENCE_PIXELS)
    if qoi_encode(packed) != _REFERENCE_STREAM:
        raise PudError("QOI encoder differs between the tuple and bytes paths")
    # 24-bit gradient -> 565 packing
    rgb = bytes([0, 0, 0, 255, 255, 255])
    if rgb888_to_rgb565(rgb, 2, 1) != bytes([0x00, 0x00, 0xFF, 0xFF]):
        raise PudError("RGB565 packing mismatch")
    got = rle_encode(_REFERENCE_PIXELS)
    if got != _RLE_REFERENCE_STREAM:
        raise PudError("RLE encoder mismatch:\n  got %s\n  want %s"
                       % (got.hex(), _RLE_REFERENCE_STREAM.hex()))
    if rle_encode(struct.pack("<8H", *_REFERENCE_PIXELS)) != _RLE_REFERENCE_STREAM:
        raise PudError("RLE encoder differs between the tuple and bytes paths")

    print("pud_usb self-test OK")
    print("  QOI encoder matches the C library reference vector")
    print("  RLE encoder matches the C library reference vector")
    try:
        import lz4.block
    except ImportError:
        print("  LZ4 encoder skipped (pip install lz4)")
    else:
        blob = lz4_encode(_REFERENCE_PIXELS)
        if lz4.block.decompress(blob, uncompressed_size=16) != \
                struct.pack("<8H", *_REFERENCE_PIXELS):
            raise PudError("LZ4 encoder does not round trip")
        print("  LZ4 encoder round trips (liblz4, via the lz4 package)")
    print("  band limit %d pixels, %d bytes per transfer"
          % (PUD_MAX_BAND_PIXELS, USB_TRANS_MAX_SIZE))
    try:
        import numpy
        print("  numpy %s present (RGB565 packing accelerated)"
              % numpy.__version__)
    except ImportError:
        print("  numpy absent (using the pure python packing path)")


if __name__ == "__main__":
    try:
        _selftest()
    except PudError as exc:
        sys.exit("FAILED: %s" % exc)
