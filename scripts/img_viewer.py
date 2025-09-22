#!/usr/bin/env python3

#
# Copyright (c) 2024 embeddedboys developers
#
# Copyright (c) 2020 Raspberry Pi (Trading) Ltd. author of https://github.com/raspberrypi/pico-examples/tree/master/usb
#
# SPDX-License-Identifier: BSD-3-Clause
#

# sudo pip3 install pyusb

import os
import sys
import cv2

import usb.core
import usb.util
import datetime

# JPEG quality for compression (1 to 100, higher is better quality)
JPEG_QUALITY = 50

EP_DIR_OUT = 0x00
EP_DIR_IN = 0X80
TYPE_VENDOR = 0X40

EP1_OUT_ADDR = (EP_DIR_OUT | 0x01)

REQ_EP0_OUT = 0X00
REQ_EP0_IN = 0X01
REQ_EP1_OUT = 0X02
REQ_EP2_IN = 0X03

# where the image will be writen to
x = 0
y = 0

def swap(a, b):
    tmp = a
    a = b
    b = tmp
    return a, b

def main():
    TARGET_WIDTH = 480
    TARGET_HEIGHT = 320

    if len(sys.argv) < 2:
        print("Usage: sudo {} [xres] [yres] <file.jpg>".format(sys.argv[0]))
        sys.exit(1)

    if len(sys.argv) == 3:
        TARGET_WIDTH = int(sys.argv[1])
    elif len(sys.argv) == 4:
        TARGET_WIDTH = int(sys.argv[1])
        TARGET_HEIGHT = int(sys.argv[2])

    hor_res = TARGET_WIDTH
    ver_res = TARGET_HEIGHT
    print(hor_res, ver_res)

    img = cv2.imread(sys.argv[-1])
    height, width, channels = img.shape
    print("Raw image size: {}x{}".format(width, height))

    # if width < height:
    #     hor_res, ver_res = swap(hor_res, ver_res)
    #     img = cv2.rotate(img, cv2.ROTATE_90_COUNTERCLOCKWISE)

    img = cv2.resize(img, (hor_res, ver_res))
    cv2.imwrite("/tmp/.preview.jpg", img, [
        cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY,
        cv2.IMWRITE_JPEG_OPTIMIZE, 1,
    ])

    with open("/tmp/.preview.jpg", "rb") as f:
        pic = f.read()
        if len(pic) % 2 != 0:
            pic = pic + b'\xff'

        size = len(pic)
        print(f"dst image size : {hor_res}x{ver_res},", size, "(Bytes)")
        control_buffer = [(x & 0xff), (x >> 8), (y & 0xff), (y >> 8), size & 0xFF, (size >> 8) & 0xFF]
        dev = usb.core.find(idVendor=0x2E8A, idProduct=0x0001)
        if dev is None:
            raise ValueError('Device not found')
        dev.ctrl_transfer(TYPE_VENDOR | EP_DIR_OUT, REQ_EP1_OUT, 0, 0, control_buffer)
        start = datetime.datetime.now()
        dev.write(EP1_OUT_ADDR, pic)
        end = datetime.datetime.now()
        elapsed = end - start
        print("frame took {} ms".format(elapsed.microseconds / 1000))

if __name__ == "__main__":
    main()
