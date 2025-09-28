#!/usr/bin/env python3

# pip install pillow numpy lz4 pyusb

from PIL import Image
import numpy as np
import lz4.block
import usb.core
import datetime
import sys
import os

EP_DIR_OUT = 0x00
EP_DIR_IN = 0X80
TYPE_VENDOR = 0X40

EP1_OUT_ADDR = (EP_DIR_OUT | 0x01)

REQ_EP0_OUT = 0X00
REQ_EP0_IN = 0X01
REQ_EP1_OUT = 0X02
REQ_EP2_IN = 0X03

x = 0
y = 0

def rgb888_to_rgb565_le(r, g, b):
    # RGB565: R(5bit) G(6bit) B(5bit)
    r, g, b = int(r), int(g), int(b)
    rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return [rgb565 & 0xFF, (rgb565 >> 8) & 0xFF]

def jpg_to_rgb565_le(jpg_path) -> list:
    img = Image.open(jpg_path).convert("RGB")
    width, height = img.size
    print(f"image width: {width}, height: {height}")
    pixels = np.array(img)

    data = bytearray()

    for y in range(height):
        for x in range(width):
            r, g, b = pixels[y, x]
            lb, hb = rgb888_to_rgb565_le(r, g, b)
            data.append(lb)
            data.append(hb)
    return [width, height, data]

def create_ep1_control_buffer(xs, ys, xe, ye, size) -> list:
    print(f"xs: {xs}, ys: {ys}, xe: {xe}, ye: {ye}, size: {size}")
    return [
        xs & 0xff, (xs >> 8) & 0xff,
        ys & 0xff, (ys >> 8) & 0xff,
        xe & 0xff, (xe >> 8) & 0xff,
        ye & 0xff, (ye >> 8) & 0xff,
        (size >> 16) & 0xFF, (size >> 24) & 0xFF,
        size & 0xFF, (size >> 8) & 0xFF
    ]

def main() -> None:
    if len(sys.argv) < 2:
        print("Usage: {} <file.jpg>".format(sys.argv[0]))
        sys.exit(1)

    width, height, data = jpg_to_rgb565_le(sys.argv[1])

    lz4_data : bytes = lz4.block.compress(data)
    lz4_data = lz4_data[4:] # Remove lz4 block header
    with open("/tmp/lz4_data.bin", "wb") as f:
        f.write(lz4_data)
        f.close()

    lz4_size = len(lz4_data)
    print("orig:", len(data), "bytes")
    print("lz4 : ", lz4_size, "bytes")
    print("ratio:", lz4_size / len(data))

    dev = usb.core.find(idVendor=0x2E8A, idProduct=0x0001)
    if dev is None:
        raise ValueError('Device not found')

    control_buffer = create_ep1_control_buffer(x, y, x + width - 1, y + height - 1, lz4_size)
    print(control_buffer)
    dev.ctrl_transfer(TYPE_VENDOR | EP_DIR_OUT, REQ_EP1_OUT, 0, 0, control_buffer)
    start = datetime.datetime.now()
    dev.write(EP1_OUT_ADDR, lz4_data)
    end = datetime.datetime.now()
    elapsed = end - start
    print("frame time : {:.2f} ms".format(elapsed.microseconds / 1000))

    pass

if __name__ == '__main__':
    main()
