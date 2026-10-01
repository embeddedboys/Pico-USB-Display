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

import collections
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
#: Gone since protocol v2: the rectangle travels in the EP1 header.  Kept here
#: so the number stays documented (the device stalls it, on purpose).
REQ_EP1_OUT = 0x02
REQ_EP2_IN = 0x03
REQ_EP4_IN = 0x05
#: Runtime parameters ride the control endpoint (protocol v2): a control OUT
#: carrying a command header plus the values.  EP3 is still unused.
REQ_SET_PARAM = 0x06

CMD_GET_SN = 0x01
CMD_GET_CAPS = 0x02
#: Which band each DECODER_TYPE 6 dictionary window holds (see the firmware's
#: struct pud_qoid_state).  A command of its own rather than a field added to
#: the caps struct: that one is a protocol field the driver parses at a fixed
#: size, and an unknown command is simply never sent.
CMD_GET_QOID = 0x05
#: magic, slots, 4x serial, 4x len, 4x valid, window = 15 u32,
#: matching struct pud_qoid_state in include/pud.h
QOID_STATE = struct.Struct("<" + "I" * 15)
CMD_SET_PARAM = 0x03
CMD_GET_PARAM = 0x04

#: Device capability report (``PUD_CMD_GET_CAPS``): magic, protocol version,
#: the largest single EP1 transfer the device accepts, its active decoder and
#: (appended later) the panel parameters.  Kept in sync with ``struct
#: pud_caps`` in the firmware and the driver; the first 16 bytes are the older
#: layout, which a firmware from before the panel block still answers with.
CAPS_MAGIC = 0x43445550  # "PUDC"

#: Capability flag: the device has a touch controller and polls it.  Most board
#: configs in pico-display-lib have no touch (``INDEV_DRV_NOT_USED=1``), so a
#: host must not assume touch just because EP4 exists.
CAPS_TOUCH = 0x0001

CAPS_V1 = struct.Struct("<IIII")
CAPS_V1_SIZE = CAPS_V1.size
CAPS_STRUCT = struct.Struct("<IIIIHHHBBBBHHH")

#: Runtime parameters (``PUD_CMD_SET_PARAM`` / ``PUD_CMD_GET_PARAM``).  A write
#: carries a mask plus the values it wants; the answer reports the values in
#: effect, the set this firmware can change at runtime, and which fields of the
#: last write it could not apply (a rejected field does not fail the write).
#: Kept in sync with ``struct pud_params`` / ``struct pud_param_state`` in the
#: firmware's ``include/pud.h`` and the driver.
PARAM_BRIGHTNESS = 0x00000001  #: u8, 0..100 percent
PARAM_ROTATION = 0x00000002  #: 0..3, TFT_ROTATION numbering
#: 0x00000004 is retired -- it was ``fps``, and the device does not pace frames
#: at all (EP1 flow control makes the host wait).  The bit and the byte it used
#: stay unused rather than being renumbered, so an implementation that already
#: knows the number is not silently misread.
PARAM_DECODER = 0x00000008  #: DECODER_TYPE numbering

PARAMS_STRUCT = struct.Struct("<IBBBB")  # mask, brightness, rotation, reserved, decoder
PARAM_STATE_STRUCT = struct.Struct("<IIBBBB")

PARAM_NAMES = {
    PARAM_BRIGHTNESS: "brightness",
    PARAM_ROTATION: "rotation",
    PARAM_DECODER: "decoder",
}


def param_names(mask):
    """Render a PUD_PARAM_* mask as a readable list of field names."""
    names = [name for bit, name in sorted(PARAM_NAMES.items()) if mask & bit]
    return ",".join(names) if names else "-"

#: Protocol version this host speaks.  v2 moved the rectangle and the payload
#: length out of the REQ_EP1_OUT control request and into a header in front of
#: every EP1 payload -- one control transfer less per band (measured 0.14 ms).
PUD_PROTO_VER = 2

#: EP1 transfer framing: this header, then the payload it describes.
EP1_HEADER = struct.Struct("<HHHHI")
EP1_HEADER_SIZE = EP1_HEADER.size

#: Largest payload the firmware accepts in one transfer (its frame slot is
#: 64 KiB, and the protocol carries the length in a 16-bit field).
#: This is the *host* ceiling; the device reports its own through
#: ``Display.frame_max`` and the smaller of the two wins.
USB_TRANS_MAX_SIZE = 65535

#: A transfer is decoded as one self-contained QOI image, so a rectangle is
#: split into horizontal bands that survive QOI's worst case of 3 bytes per
#: pixel plus the 8-byte header and 8-byte end marker, inside a transfer that
#: also carries EP1_HEADER_SIZE bytes of framing.  Same rule the driver uses
#: (PUD_MAX_BAND_PIXELS), which costs nothing in sustained throughput.
PUD_MAX_BAND_PIXELS = (USB_TRANS_MAX_SIZE - EP1_HEADER_SIZE - 16) // 3

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


def qoiz_encode(pixels, level=6):
    """QOI, then raw deflate (RFC 1951, no zlib header) over the QOI stream.

    The device (DECODER_TYPE 5) inflates the transfer and QOI-decodes the result,
    so the inner stream is exactly what `qoi_encode` produces.

    Level 6 is the default because the level is most of the gain: on the
    synthetic desktop, banded the way an RP2350 asks for, level 1 takes 11.2% off
    the QOI bytes and level 6 takes 26.6% (level 9: 28.2%).  With the frame on
    the link's limit that is the frame time too -- measured end to end, full
    480x320 screen, one instrument for every row (notes/decoders.md):

      QOI                93893 B   101.06 ms
      QOI+deflate L1     83361 B    92.50 ms   -8.5%
      QOI+deflate L6     68874 B    77.99 ms   -22.8%
      QOI+deflate L6, 3 frame slots   73.65 ms -27.1%  (= the link, no decoder)

    The cost of the higher level is on the host: encoding a full frame is about
    10 ms at level 1 and 40-55 ms at level 6.  A kernel driver that must encode
    inside a frame deadline should weigh that, and a level-1 stream is still
    decodable by the same device -- the level is not a protocol field.  zlib is
    the same deflate the kernel has built in (lib/zlib_deflate), so no encoder
    has to be vendored for it.
    """
    import zlib

    comp = zlib.compressobj(level, zlib.DEFLATED, -15)
    return comp.compress(qoi_encode(pixels)) + comp.flush()


# ---------------------------------------------------------------------------
# RGB565 QOI + cross-frame dictionary deflate (DECODER_TYPE 6)
#
# The sub-header rides *inside* the EP1 payload, i.e. after the 12 byte
# `struct pud_ep1_header`, so no shared protocol field changes:
#
#      0  u32  magic        0x44445550, which is 'P','U','D','D' -- the bytes
#                           on the wire are 50 55 44 44 (little endian)
#      4  u16  flags        bit0 DELTA (the deflate stream had a preset dict)
#                           bit1 KEYFRAME (this band resets the slot history)
#      6  u16  reserved     0
#      8  u32  dict_serial  DELTA: the accepted-strip serial the slot's history
#                           was recorded with
#     12  u32  dict_len     DELTA: history bytes the encoder used as zdict
#     16  ...  raw deflate  stream (RFC 1951, no zlib header, fixed Huffman)
#
# 16 bytes, 4 byte aligned, every multi-byte field little endian.  `size` in
# `struct pud_ep1_header` is the payload size *including* this sub-header.
# ---------------------------------------------------------------------------

QOID_MAGIC = 0x44445550
QOID_FLAG_DELTA = 0x0001
QOID_FLAG_KEYFRAME = 0x0002
QOID_SUBHEADER = struct.Struct("<IHHII")
QOID_SUBHEADER_SIZE = QOID_SUBHEADER.size
#: magic of the PUD_CMD_GET_QOID answer (PUD_QOID_MAGIC in the firmware), which
#: is a different word from the sub-header magic above
QOID_STATE_MAGIC = 0x51445550
QOID_WINDOWS = 4

#: Per frame slot, and split in half: history | output.  A cache variable in
#: the firmware (PUD_DELTA_WIN, 32 KB on RP2350 / 16 KB on RP2040), so a band's
#: QOI -- and the history it is deflated against -- has to fit in half of it.
#: `Display.delta_win` carries the value the host builds against.
PUD_DELTA_WIN = 32 * 1024

#: Device frame slots (DECODER_FRAME_SLOTS in the firmware).  Slots are handed
#: out round-robin, so the k-th submission of a session lands in slot
#: k % DECODER_FRAME_SLOTS and finds the band submitted that many submissions
#: earlier.  `Display.frame_slots` carries it.
DECODER_FRAME_SLOTS = 3


def qoid_pack(qoi, history=None, dict_serial=0, level=6):
    """Frame one QOI stream as a `DECODER_TYPE 6` payload.

    A 16 byte sub-header, then a raw deflate stream (RFC 1951, no zlib header)
    over `qoi`.  The strategy is fixed Huffman (`Z_FIXED`) because that is the
    format the device implements: `tinyd` has stored + fixed Huffman and
    *refuses* dynamic tables instead of decoding them wrong.

    `history` is the QOI stream of the same band as the previous frame left it,
    and presets the deflate dictionary -- this is the whole point of the codec.
    `dict_serial` is the accepted-strip serial the host believes that history
    was recorded with; the device drops a DELTA whose serial or dict_len does
    not match the slot, because a wrong dictionary decodes to *wrong pixels*
    with rc 0 rather than failing (see notes/decoders.md).

    Without a history this is a KEYFRAME: dict_len 0, serial irrelevant.  The
    device stores whatever it decodes as the slot's new history either way, so
    a keyframe is always legal and is also how a host recovers a lost ring.

    Works on QOI bytes rather than pixels so the caller can keep them for the
    next frame's dictionary without encoding the band twice; `qoid_encode` is
    the pixels-in version.
    """
    import zlib

    if history:
        flags, dict_len = QOID_FLAG_DELTA, len(history)
    else:
        flags, dict_len, dict_serial = QOID_FLAG_KEYFRAME, 0, 0

    comp = zlib.compressobj(level, zlib.DEFLATED, -15, 8, zlib.Z_FIXED,
                            zdict=history or b"")
    return (QOID_SUBHEADER.pack(QOID_MAGIC, flags, 0, dict_serial, dict_len)
            + comp.compress(qoi) + comp.flush())


def qoid_encode(pixels, history=None, dict_serial=0, level=6):
    """QOI, then raw deflate against a *cross-frame* dictionary
    (`DECODER_TYPE 6`).

    Frame-internal deflate (see `qoiz_encode`) cannot see what the previous
    frame already put on the panel, and that is most of a desktop's redundancy:
    the same glyph, the same unchanged UI, the wallpaper behind a moved window.
    QOI encodes the band, then the band is deflated with the *previous frame's
    QOI for the same band* preset as the dictionary.  Measured host side on the
    synthetic desktop (notes/decoders.md, "跨帧字典"):

      dirty bands, larger change    QOI 32744 B -> 18939 B   -42.2%
      dirty bands, small change     QOI 32744 B ->  6899 B   -78.9%
      one UI element in one band    QOI 11270 B ->  3464 B   -69.0%

    The gain is proportional to how much of the band did not change, which is
    exactly the shape of a partial refresh.  The cost is on the device, which
    is why this codec only makes sense on a chip whose CPU is idle: the slot
    keeps the last band's QOI and the inflate is preset with it, measured at
    15.9 cycles per output byte at 150~384 MHz (flat in clock), i.e. ~8 ms for
    a full 480x320 frame at 225 MHz -- hidden behind a 45~74 ms transfer.

    `history` is that dictionary: the QOI bytes the device's frame slot still
    holds.  It is *state*, so `Display.send_rgb565(codec="qoid")` owns it (one
    history per frame slot, slots round-robin); with no history -- including
    the generic `ENCODERS["qoid"](band)` call, where one band cannot know what
    the slot holds -- this is a keyframe.
    """
    return qoid_pack(qoi_encode(pixels), history, dict_serial, level)


#: The encoders a host may send with.  The device has to be built for the same
#: one (DECODER_TYPE); `Display.query_caps()` asks which.
ENCODERS = {
    "qoi": qoi_encode,
    "rle": rle_encode,
    "lz4": lz4_encode,
    "qoiz": qoiz_encode,
    "qoid": qoid_encode,
}

#: The DECODER_TYPE each of those needs on the device -- a protocol field
#: (`PUD_CMD_GET_CAPS` reports the device's own, see notes/usb-protocol.md).
#: 0 is tjpgd, 1 is JPEGDEC (both JPEG, the host only ever sends whole frames);
#: 3 QOI and 4 RLE are the default paths, 2 is LZ4 (banded, see lz4_encode),
#: 5 is QOI + deflate (experimental, see qoiz_encode), 6 is QOI + a cross-frame
#: dictionary (experimental, see qoid_encode -- it needs per-slot host state,
#: so it is not usable through send_raw()).
DECODER_TYPES = {
    "jpeg": 1,
    "lz4": 2,
    "qoi": 3,
    "rle": 4,
    "qoiz": 5,  # experimental: QOI + raw deflate
    "qoid": 6,  # experimental: QOI + cross-frame dictionary deflate
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
    try:
        disp.query_caps()
    except PudError:
        # A protocol mismatch is not something to paper over: the transfer
        # framing differs, so say so instead of failing on a mystery timeout.
        raise
    except Exception:
        # No capability report (an older firmware): keep the host defaults.
        pass
    return disp


class Display:
    """A claimed device; use as a context manager."""

    def __init__(self, dev, width=480, height=320, delta_win=PUD_DELTA_WIN,
                 frame_slots=DECODER_FRAME_SLOTS):
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

        # DECODER_TYPE 6 (qoid) state.  The dictionary lives on the device, one
        # per frame slot, so the host has to mirror which band each slot holds:
        # `_qoid_ring` is the last `frame_slots` submissions (rectangle, QOI
        # bytes, serial) and `qoid_serial` counts the bands accepted.  Both are
        # session state -- only send_rgb565(codec="qoid") keeps them, so mixing
        # send_raw() in between desynchronizes the serial and the device starts
        # dropping the deltas (visible in its counters, not a corrupt picture).
        self.delta_win = delta_win
        self.frame_slots = max(1, frame_slots)
        self.reset_qoid_state()

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
    def _payload_limit(self):
        """Largest payload one transfer can carry, i.e. what is left of the
        device's (and the host's) transfer ceiling after the EP1 header."""
        return min(USB_TRANS_MAX_SIZE, self.frame_max) - EP1_HEADER_SIZE

    def _send_rect(self, xs, ys, xe, ye, payload, timeout):
        """One EP1 transfer: the header, then the payload it describes.

        The declared length is rounded up to even and the payload padded with a
        zero byte to match.  The device does not require that any more -- a
        header with an odd `size` is accepted (measured 2026-09 on RP2350: four
        odd and four even payloads from 987 to 43271 bytes, all landed with
        got == total) -- but firmware built before that check was dropped throws
        an odd size away, counts it in g_ep1_stat.oversize and the host sees
        *no error at all*.  Padding costs one byte and keeps this client working
        against either firmware; pud_flush() in the kernel driver rounds the
        same way.  QOI decoding stops at its end marker, so the trailing byte is
        never consumed.
        """
        if len(payload) & 1:
            payload = payload + b"\x00"
        self.dev.write(EP1_OUT_ADDR,
                       EP1_HEADER.pack(xs, ys, xe, ye, len(payload)) + payload,
                       timeout=timeout)

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
        limit = self._payload_limit()
        if len(payload) > limit:
            raise PudError(
                "%d byte payload exceeds the %d byte limit this device accepts"
                % (len(payload), limit))
        self._check_rect(xs, ys, xe, ye)
        t0 = time.perf_counter()
        self._send_rect(xs, ys, xe, ye, payload, timeout)
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
          qoid        the band is one QOI stream deflated against the frame
                      slot's cross-frame dictionary (see qoid_encode), so this
                      method also owns the slot bookkeeping: the first
                      `frame_slots` submissions of a session are keyframes, and
                      later ones are deltas only if the slot still holds this
                      very rectangle from `frame_slots` submissions ago

        Returns (bands, payload_bytes, seconds).
        """
        try:
            encode = ENCODERS[codec]
        except KeyError:
            raise PudError("unknown codec %r (have: %s)"
                           % (codec, ", ".join(sorted(ENCODERS))))

        if codec == "qoid" and self.decoder_type not in (None,
                                                         DECODER_TYPES["qoid"]):
            # Every other codec's stream is self-describing to its own decoder;
            # a DECODER_TYPE 6 payload is not, and a QOI decoder fed the 16 byte
            # sub-header draws garbage. Refuse instead of corrupting the panel.
            raise PudError("device reports decoder_type=%s, qoid needs %d"
                           % (self.decoder_type, DECODER_TYPES["qoid"]))

        timeout = timeout or DEFAULT_TIMEOUT_MS
        self._check_rect(xs, ys, xs + width - 1, ys + height - 1)
        rows = max(1, self.band_pixels // width)
        limit = self._payload_limit()
        total = 0
        bands = 0
        t0 = time.perf_counter()

        for y in range(0, height, rows):
            bh = min(rows, height - y)
            start = y * width * 2
            band = rgb565[start:start + bh * width * 2]
            rect = (xs, ys + y, xs + width - 1, ys + y + bh - 1)
            qoi = None
            if codec == "qoid":
                # The payload depends on what the device's slot still holds, so
                # it is built here with the ring and the serial in hand.
                payload, qoi = self._qoid_band(band, rect, limit)
            else:
                payload = encode(band)
                if len(payload) > limit:
                    raise PudError("band of %d bytes exceeds the %d byte limit"
                                   % (len(payload), limit))
            self._send_rect(rect[0], rect[1], rect[2], rect[3], payload,
                            timeout)
            if qoi is not None:
                self._qoid_accept(rect, qoi)
            total += len(payload)
            bands += 1

        return bands, total, time.perf_counter() - t0

    # -- DECODER_TYPE 6 (qoid): the cross-frame dictionary ------------------
    #
    # The dictionary is device state, one per frame slot, and slots are handed
    # out round-robin: the k-th band of a session lands in slot
    # k % frame_slots, which still holds the band submitted `frame_slots`
    # submissions earlier.  The host mirrors that with `_qoid_ring`, so it can
    # only build a delta for a band whose history it still has, for the same
    # rectangle, and only while its own count of accepted bands still lines up
    # with the device's.  Whenever it cannot, the band goes as a keyframe,
    # which is always legal and resets the slot.
    #
    # `qoid_serial` counts the bands accepted in this session, which is also
    # the serial the device records for the band being sent.  It is only right
    # for a fresh device session: after a device reset the host is off by
    # whatever the device lost, the deltas it sends carry a serial and a
    # dict_len the slot does not have, and the device drops them as
    # g_decoder_stat_qoid_mismatch -- a countable, visible failure rather than
    # a corrupted picture.  Recovery is to keyframe from a known baseline:
    # reset_qoid_state() plus a device reset (the spec's one-off "align the
    # counter by control request" is not in the protocol yet, so the host has
    # nothing better to align against).

    def reset_qoid_state(self):
        """Forget the device's frame-slot history for `DECODER_TYPE 6`.

        The device counts accepted bands from its own boot and keeps the last
        band per slot, so this is what puts the host back on that baseline: the
        next `frame_slots` submissions go as keyframes.  Call it after a device
        reset (or before sending a second, unrelated session over one claim).
        """
        self.qoid_serial = 0
        self._qoid_ring = collections.deque(maxlen=16)
        self._qoid_oversize_warned = False

    def query_qoid_state(self, timeout=None):
        """Which band each dictionary window holds (``PUD_CMD_GET_QOID``).

        Returns ``{serial: qoi_length}`` for the windows that hold a band, or
        None when the device does not answer (an older firmware leaves its
        previous answer in the buffer, so the magic decides, not the length).
        """
        if getattr(self, "dev", None) is None:
            # no device behind this Display (the offline self-test drives the
            # band path directly): nothing is resident, so say so and let the
            # caller send a keyframe rather than guess
            return None
        timeout = timeout or DEFAULT_TIMEOUT_MS
        self.dev.ctrl_transfer(
            TYPE_VENDOR | EP_DIR_OUT, REQ_EP2_IN, 0, 0,
            struct.pack("<HH", CMD_GET_QOID, QOID_STATE.size))
        raw = bytes(self.dev.read(EP2_IN_ADDR, QOID_STATE.size,
                                  timeout=timeout))
        if len(raw) < QOID_STATE.size:
            return None
        fields = QOID_STATE.unpack(raw)
        if fields[0] != QOID_STATE_MAGIC:
            return None
        slots = min(fields[1], QOID_WINDOWS)
        serial, length, valid = fields[2:2 + slots], \
            fields[6:6 + slots], fields[10:10 + slots]
        window = fields[14]
        if window:
            # stop guessing the window size: it is a build choice (32 KB on
            # RP2350, 16 KB on RP2040) and a band larger than half of it is
            # refused by the device as oversize, i.e. that part of the panel
            # silently stops updating
            self.delta_win = window
        return dict((serial[i], length[i]) for i in range(slots)
                    if valid[i])

    def _qoid_history(self, rect):
        """(qoi, serial) to prime the encoder with, or None for a keyframe.

        This predicts the window rather than asking: the device checks a delta
        against this band's own slot, which holds the band submitted
        `frame_slots` ago only while the pipeline is saturated (measured: with a
        slow host every band lands in one slot, and 8 of 15 deltas were
        refused).  A wrong guess is refused and counted by the device -- never
        painted wrong -- but that band does not update, so this is the weaker
        mode.

        `query_qoid_state()` is what replaces the guess, and it is deliberately
        *not* called from here yet: it is a control transfer, and interleaving
        one between the bands of a frame is a change to the data path that has
        to be shown harmless on its own first (notes/todo.md item 13).
        """
        # The newest band with this rectangle is both the strongest dictionary
        # (least has changed since) and the one most likely to still be in a
        # window, since only the last `frame_slots` accepted bands can be.  The
        # old rule named whatever was exactly `frame_slots` submissions back --
        # the round-robin window -- which a slow host never fills that way.
        # A dictionary has to be a band the decoder has *finished*, or the
        # device's windows do not hold it yet and the delta is refused -- which
        # costs that band its update.  Decoding is asynchronous and one task
        # deep, so a band is safe at exactly `frame_slots` submissions back:
        # flow control lets at most that many be in flight, and that band is
        # then both decoded and the oldest of the resident set.  Measured: with
        # the immediately previous band named instead, a host fast enough to
        # outrun the decoder (a delta is 0.18 ms on the wire) had 17 of 20
        # deltas refused.
        # `frame_slots` back is the best any fixed distance can do, and it is
        # not good enough: there the band may still be finishing (measured 5 of
        # 20 deltas refused by a host that outran the decoder), while one
        # further back is often already evicted (8 of 20).  With `frame_slots`
        # windows and a `frame_slots` deep pipeline the two windows do not
        # overlap, so residency cannot be predicted at all -- it has to be
        # asked for (query_qoid_state) or avoided (a host that paces itself:
        # 12 of 12 accepted with 50 ms between bands, 96 of 96 on a full-screen
        # resend).  See notes/todo.md item 13.
        safe = self.qoid_serial - self.frame_slots
        fallback = None
        for band_rect, qoi, serial in reversed(self._qoid_ring):
            if serial > safe:
                continue            # may still be in flight: not in a window yet
            if band_rect == rect:
                return qoi, serial  # newest eligible band with this rectangle
            if fallback is None:
                fallback = (qoi, serial)    # newest eligible band at all
        # Naming a different rectangle is legitimate: the device searches its
        # windows and checks the length, so correctness does not depend on the
        # rectangles matching -- only the compression ratio does, and a
        # vertically adjacent band shares a desktop's background and chrome.
        # This is also the only way a full-screen resend gets a dictionary at
        # all, since every band in it is a different rectangle.
        return fallback

    def _qoid_band(self, band, rect, limit):
        """(payload, qoi) for one band: a DELTA when the slot's history is
        usable, a KEYFRAME otherwise.

        `qoi` is handed back so the caller can record it as a slot history once
        the transfer has actually gone out (see _qoid_accept).  A payload that
        does not fit the transfer limit is never truncated: the delta falls
        back to a keyframe, and a keyframe that does not fit is an error.
        """
        qoi = qoi_encode(band)
        history = self._qoid_history(rect)
        half = self.delta_win // 2
        if history is not None and (len(history[0]) > half or len(qoi) > half):
            # The window is history | output, half each, so both sides of a
            # delta have to fit in half of it; the device drops the band as
            # oversize otherwise (g_decoder_stat_delta_oversize, no truncation).
            history = None
        if history is not None:
            payload = qoid_pack(qoi, history[0], history[1])
            if len(payload) <= limit:
                return payload, qoi
            # too big on the wire: the keyframe below is the smaller one anyway
        payload = qoid_pack(qoi)
        if len(payload) > limit:
            raise PudError(
                "band QOI of %d bytes is %d bytes with the DECODER_TYPE 6 "
                "sub-header, over the %d byte limit this device accepts"
                % (len(qoi), len(payload), limit))
        if len(qoi) > half and not self._qoid_oversize_warned:
            # Photo content: the band is fine on the link but the device cannot
            # hold it. Say so once -- it drops it as oversize, invisibly.
            self._qoid_oversize_warned = True
            sys.stderr.write(
                "qoid: band QOI of %d bytes exceeds the %d byte half window, "
                "the device will drop it as oversize\n" % (len(qoi), half))
        return payload, qoi

    def _qoid_accept(self, rect, qoi):
        """Record a band the device has accepted, with the serial it gets."""
        self._qoid_ring.append((rect, qoi, self.qoid_serial))
        self.qoid_serial += 1

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

        Updates ``frame_max``, ``band_pixels``, ``decoder_type`` and, when the
        firmware reports them, the panel parameters.  A device without the
        command answers with whatever was left in its buffer, so the magic (not
        the transfer length) is what decides; on any mismatch the conservative
        defaults are kept and None is returned.  A firmware predating the panel
        parameters answers with the first 16 bytes and is accepted.
        """
        timeout = timeout or DEFAULT_TIMEOUT_MS
        self.dev.ctrl_transfer(
            TYPE_VENDOR | EP_DIR_OUT, REQ_EP2_IN, 0, 0,
            struct.pack("<HH", CMD_GET_CAPS, CAPS_STRUCT.size))
        raw = bytes(self.dev.read(EP2_IN_ADDR, CAPS_STRUCT.size,
                                  timeout=timeout))
        if len(raw) < CAPS_V1_SIZE:
            return None
        magic, proto_ver, frame_max, decoder_type = CAPS_V1.unpack(
            raw[:CAPS_V1_SIZE])
        if magic != CAPS_MAGIC or not (0 < frame_max <= (1 << 20)):
            return None
        if proto_ver != PUD_PROTO_VER:
            raise PudError(
                "device speaks protocol v%d, this host speaks v%d -- the "
                "rectangle now travels in the EP1 header, so update whichever "
                "side is older" % (proto_ver, PUD_PROTO_VER))

        self.caps = dict(proto_ver=proto_ver, frame_max=frame_max,
                         decoder_type=decoder_type)
        if len(raw) >= CAPS_STRUCT.size:
            (_magic, _proto, _frame_max, _decoder, xres, yres, pixelclock_khz,
             rotation, bpp, intf_type, tp_polling_period,
             width_mm, height_mm, flags) = CAPS_STRUCT.unpack(raw)
            self.caps.update(xres=xres, yres=yres, bpp=bpp or 16,
                             rotation=rotation,
                             pixelclock_khz=pixelclock_khz,
                             intf_type=intf_type,
                             touch_polling_period=tp_polling_period,
                             width_mm=width_mm, height_mm=height_mm,
                             touch=bool(flags & CAPS_TOUCH))
        self.frame_max = min(USB_TRANS_MAX_SIZE, frame_max)
        if self.frame_max > EP1_HEADER_SIZE + 16:
            self.band_pixels = max(
                1, (self.frame_max - EP1_HEADER_SIZE - 16) // 3)
        self.decoder_type = decoder_type
        return self.caps

    def set_params(self, mask=0, brightness=0, rotation=0, decoder=0):
        """Write runtime parameters (``PUD_CMD_SET_PARAM``).

        Only the fields named in ``mask`` are touched.  The device does not
        answer this write: a field it cannot apply comes back in ``rejected``
        from :meth:`get_params`.  A malformed write (short payload, wrong
        command) is stalled, which pyusb raises as a ``USBError``.
        """
        payload = PARAMS_STRUCT.pack(mask, brightness, rotation, 0, decoder)
        self.dev.ctrl_transfer(
            TYPE_VENDOR | EP_DIR_OUT, REQ_SET_PARAM, 0, 0,
            struct.pack("<HH", CMD_SET_PARAM, len(payload)) + payload,
            timeout=DEFAULT_TIMEOUT_MS)

    def get_params(self, timeout=None):
        """Read the runtime parameters in effect (``PUD_CMD_GET_PARAM``).

        Returns ``settable``/``rejected`` masks plus the current values.
        """
        timeout = timeout or DEFAULT_TIMEOUT_MS
        self.dev.ctrl_transfer(
            TYPE_VENDOR | EP_DIR_OUT, REQ_EP2_IN, 0, 0,
            struct.pack("<HH", CMD_GET_PARAM, PARAM_STATE_STRUCT.size))
        raw = bytes(self.dev.read(EP2_IN_ADDR, PARAM_STATE_STRUCT.size,
                                  timeout=timeout))
        if len(raw) < PARAM_STATE_STRUCT.size:
            raise PudError(
                "PUD_CMD_GET_PARAM answered %d of %d bytes -- firmware without "
                "runtime parameters?" % (len(raw), PARAM_STATE_STRUCT.size))
        settable, rejected, brightness, rotation, _reserved, decoder = \
            PARAM_STATE_STRUCT.unpack(raw)
        return dict(settable=settable, rejected=rejected, brightness=brightness,
                    rotation=rotation, decoder=decoder)


# ---------------------------------------------------------------------------
# Self test
# ---------------------------------------------------------------------------

_REFERENCE_PIXELS = (0x1234, 0x1235, 0x1236, 0x1236, 0x1236, 0xF800, 0x07E0,
                     0x1234)
_RLE_REFERENCE_STREAM = bytes.fromhex(
    "0800000001341235128236120200f8e0073412")
_REFERENCE_STREAM = bytes.fromhex(
    "7135363508000000fe34126b6bc1fe00f876270000000000000001")

#: DECODER_TYPE 6 sub-header vectors -- the 16 bytes the device parses, with
#: `_REFERENCE_STREAM` (27 bytes) as the history, so the magic, the two flag
#: bits, the reserved field, the endianness of both u32 fields and the fact
#: that dict_len is the *history* length are pinned rather than assumed.  The
#: deflate stream behind them is zlib's output and is not pinned.
_REFERENCE_QOID_KEYFRAME = bytes.fromhex("50554444020000000000000000000000")
_REFERENCE_QOID_DELTA = bytes.fromhex("5055444401000000050000001b000000")


def _selftest_qoid():
    """`DECODER_TYPE 6` (QOI + cross-frame dictionary deflate), no hardware.

    The format rests on four things, and all four are checked here: a keyframe
    round trips, a delta round trips through `decompressobj(-15, zdict=...)`, a
    delta decoded against the *wrong* history does not come back (zlib may
    refuse it outright, but `tinyd` on the device decodes garbage with rc 0 --
    which is why the firmware has to check the serial and the length itself),
    and the slot arithmetic keyframes exactly the bands whose history the host
    cannot know.  The last part is replayed through the real band path, ending
    in a simulated device that holds the slot histories and inflates with them.
    """
    import zlib

    if QOID_STATE.size != 60:
        raise PudError("the PUD_CMD_GET_QOID answer is %d bytes here and 60 in "
                       "the firmware" % QOID_STATE.size)
    if QOID_SUBHEADER.size != 16:
        raise PudError("qoid sub-header is %d bytes, the format says 16"
                       % QOID_SUBHEADER.size)

    keyframe = qoid_pack(_REFERENCE_STREAM)
    if keyframe[:QOID_SUBHEADER_SIZE] != _REFERENCE_QOID_KEYFRAME:
        raise PudError("qoid keyframe sub-header mismatch:\n  got %s\n  want %s"
                       % (keyframe[:QOID_SUBHEADER_SIZE].hex(),
                          _REFERENCE_QOID_KEYFRAME.hex()))
    delta = qoid_pack(_REFERENCE_STREAM, _REFERENCE_STREAM, dict_serial=5)
    if delta[:QOID_SUBHEADER_SIZE] != _REFERENCE_QOID_DELTA:
        raise PudError("qoid delta sub-header mismatch:\n  got %s\n  want %s"
                       % (delta[:QOID_SUBHEADER_SIZE].hex(),
                          _REFERENCE_QOID_DELTA.hex()))

    # a band no frame-local codec can squeeze -- an LCG stands in for the photo
    # or text content that is incompressible inside one band -- of which the
    # next frame changed 24 pixels: the cross-frame case, where only the
    # dictionary helps
    band = []
    seed = 0x1234
    for _ in range(224):
        seed = (seed * 1103515245 + 12345) & 0xFFFFFFFF
        band.append((seed >> 11) & 0xFFFF)
    later = band[:-24] + [0xF81F] * 24
    qoi_a, qoi_b = qoi_encode(band), qoi_encode(later)
    kf = qoid_pack(qoi_b)
    if zlib.decompress(kf[QOID_SUBHEADER_SIZE:], -15) != qoi_b:
        raise PudError("qoid keyframe does not round trip to the QOI stream")
    df = qoid_pack(qoi_b, qoi_a, dict_serial=1)
    if zlib.decompressobj(-15, zdict=qoi_a).decompress(
            df[QOID_SUBHEADER_SIZE:]) != qoi_b:
        raise PudError("qoid delta does not round trip against its history")
    if len(df) >= len(kf):
        raise PudError("qoid delta (%d B) is not smaller than the keyframe "
                       "(%d B) on a band that barely changed"
                       % (len(df), len(kf)))

    # the wrong history must not decode to the right bytes
    try:
        wrong = zlib.decompressobj(-15, zdict=qoi_encode([0x001F] * len(band)))
        wrong = wrong.decompress(df[QOID_SUBHEADER_SIZE:])
    except zlib.error:
        wrong = None       # zlib gives up; the device's tinyd does not
    if wrong == qoi_b:
        raise PudError("a qoid delta decoded against the wrong history came "
                       "out identical -- the firmware's serial/dict_len check "
                       "would not be load-bearing")

    # the band path itself: two frames of DECODER_FRAME_SLOTS bands, so every
    # slot gets the same rectangle back on the second frame
    disp = Display.__new__(Display)
    disp.delta_win = PUD_DELTA_WIN
    disp.frame_slots = DECODER_FRAME_SLOTS
    disp.reset_qoid_state()
    limit = USB_TRANS_MAX_SIZE - EP1_HEADER_SIZE
    band_px = 24
    rects = [(0, y, band_px - 1, y) for y in range(DECODER_FRAME_SLOTS)]
    first = [[0x1000 + y * 0x111] * band_px for y in range(len(rects))]
    second = [row[:] for row in first]
    second[1][-4:] = [0x2222] * 4
    slots = [None] * DECODER_FRAME_SLOTS      # (serial, history) per window
    # stand in for PUD_CMD_GET_QOID: the host cannot know residency on its own,
    # which is the whole reason the query exists
    disp.query_qoid_state = lambda timeout=None: dict(
        (w[0], len(w[1])) for w in slots if w)
    accepted = 0
    counts = {QOID_FLAG_KEYFRAME: 0, QOID_FLAG_DELTA: 0}
    for frame in (first, second):
        for rect, pixels_ in zip(rects, frame):
            raw = struct.pack("<%dH" % band_px, *pixels_)
            payload, qoi = disp._qoid_band(raw, rect, limit)
            magic, flags, _reserved, serial, dict_len = \
                QOID_SUBHEADER.unpack_from(payload)
            if magic != QOID_MAGIC:
                raise PudError("qoid payload magic is %08x, want %08x"
                               % (magic, QOID_MAGIC))
            counts[flags] = counts.get(flags, 0) + 1
            # the firmware searches its windows for the band the payload names
            held = None
            if not (flags & QOID_FLAG_KEYFRAME):
                for w in slots:
                    if w and w[0] == serial and len(w[1]) == dict_len:
                        held = w
                        break
            if flags & QOID_FLAG_KEYFRAME:
                if dict_len or serial:
                    raise PudError("qoid keyframe carries dict_len %d, serial %d"
                                   % (dict_len, serial))
                history = b""
            else:
                # what the firmware checks before it inflates
                if held is None:
                    raise PudError(
                        "qoid delta at submission %d names band %d, which no "
                        "window holds" % (accepted, serial))
                history = held[1]
            got = zlib.decompressobj(-15, zdict=history).decompress(
                payload[QOID_SUBHEADER_SIZE:])
            if got != qoi:
                raise PudError("qoid band %s did not decode back to the QOI "
                               "bytes it was built from" % (rect,))
            # a delta leaves its history in the window it decoded against, a
            # keyframe in this band's own frame slot
            if held is not None:
                slots[slots.index(held)] = (accepted, qoi)
            else:
                slots[accepted % DECODER_FRAME_SLOTS] = (accepted, qoi)
            disp._qoid_accept(rect, qoi)
            accepted += 1
    # The first band has nothing resident to diff against, so it is a keyframe;
    # after that a band may diff against any resident band, *including one from a
    # different rectangle* -- which is legitimate (correctness comes from the
    # serial and length the device checks) and is what lets a full-screen resend
    # use a dictionary at all.  So the counts are bounded, not fixed.
    if not counts[QOID_FLAG_KEYFRAME] or \
            counts[QOID_FLAG_DELTA] < DECODER_FRAME_SLOTS:
        raise PudError("qoid sent %d keyframes and %d deltas over two frames "
                       "of %d bands, want >=1 and >=%d"
                       % (counts[QOID_FLAG_KEYFRAME], counts[QOID_FLAG_DELTA],
                          DECODER_FRAME_SLOTS, DECODER_FRAME_SLOTS))

    # nothing resident, and a band too big for half the window, both have to be
    # keyframes; a rectangle nobody holds is no longer in that list, because the
    # dictionary only has to be a band the device still has
    raw = struct.pack("<%dH" % band_px, *first[0])
    disp.reset_qoid_state()         # nothing sent yet, so nothing is resident
    cold, _qoi = disp._qoid_band(raw, rects[0], limit)
    if QOID_SUBHEADER.unpack_from(cold)[1] != QOID_FLAG_KEYFRAME:
        raise PudError("qoid sent a delta with nothing resident to diff against")
    disp._qoid_oversize_warned = True    # this is what a real session prints
    disp.delta_win = 8                   # half a window is smaller than a band
    big, _qoi = disp._qoid_band(raw, rects[0], limit)
    if QOID_SUBHEADER.unpack_from(big)[1] != QOID_FLAG_KEYFRAME:
        raise PudError("qoid sent a delta for a band that cannot fit the "
                       "half window the device keeps its history in")

    print("  QOI+dict (%d) sub-header matches the reference bytes (keyframe "
          "and delta)" % DECODER_TYPES["qoid"])
    print("  QOI+dict delta %d B vs keyframe %d B (QOI %d B) on a band that "
          "barely changed, both round trip" % (len(df), len(kf), len(qoi_b)))
    print("  QOI+dict wrong-history decode is not the band (the device's "
          "serial/dict_len check is load-bearing)")
    print("  QOI+dict slots: %d keyframes then %d deltas over two frames of "
          "%d bands, every band inflates back to its QOI bytes"
          % (counts[QOID_FLAG_KEYFRAME], counts[QOID_FLAG_DELTA],
             DECODER_FRAME_SLOTS))


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
    import zlib
    if zlib.decompress(qoiz_encode(_REFERENCE_PIXELS), -15) != _REFERENCE_STREAM:
        raise PudError("QOI+deflate encoder does not round trip to the QOI stream")
    print("  QOI+deflate encoder round trips to the QOI reference stream")
    _selftest_qoid()
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
