#!/usr/bin/env python3

#
# Copyright (c) 2020 2024 Daniel Gorbea
#
# Copyright (c) 2020 Raspberry Pi (Trading) Ltd. author of https://github.com/raspberrypi/pico-examples/tree/master/usb
#
# SPDX-License-Identifier: BSD-3-Clause
#

# sudo pip3 install pyusb

import os
import sys
import time

import cv2

import usb.core
import usb.util
import datetime

from Xlib import X, display
import numpy as np

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

def create_ep1_control_buffer(xs, ys, xe, ye, size) -> list:
    # print(f"xs: {xs}, ys: {ys}, xe: {xe}, ye: {ye}, size: {size}")
    return [
        xs & 0xff, (xs >> 8) & 0xff,
        ys & 0xff, (ys >> 8) & 0xff,
        xe & 0xff, (xe >> 8) & 0xff,
        ye & 0xff, (ye >> 8) & 0xff,
        (size >> 16) & 0xFF, (size >> 24) & 0xFF,
        size & 0xFF, (size >> 8) & 0xFF
    ]

def get_screen_pixels():
    d = display.Display()
    root = d.screen().root
    geom = root.get_geometry()
    width, height = geom.width, geom.height

    # 获取原始像素数据（X11 默认格式为 BGRA）
    raw_data = root.get_image(0, 0, width, height, X.ZPixmap, 0xffffffff).data

    # 转换为 numpy 数组 (height, width, 4)
    frame = np.frombuffer(raw_data, dtype=np.uint8).reshape((height, width, 4))

    # 提取 RGB 通道（丢弃 Alpha 通道）
    rgb_frame = frame[:, :, :3]  # shape: (height, width, 3)
    return rgb_frame

def main():
    TARGET_WIDTH = 480
    TARGET_HEIGHT = 320
    JPEG_QUALITY = 50

    print("Usage: sudo {} [xres] [yres] [quality]".format(sys.argv[0]))

    if len(sys.argv) == 2:
        TARGET_WIDTH = int(sys.argv[1])
    elif len(sys.argv) == 3:
        TARGET_WIDTH = int(sys.argv[1])
        TARGET_HEIGHT = int(sys.argv[2])
    elif len(sys.argv) == 4:
        TARGET_WIDTH = int(sys.argv[1])
        TARGET_HEIGHT = int(sys.argv[2])
        JPEG_QUALITY = int(sys.argv[3])

    hor_res = TARGET_WIDTH
    ver_res = TARGET_HEIGHT
    print(f"xres:{hor_res}, yres:{ver_res}, quality:{JPEG_QUALITY}")

    dev = usb.core.find(idVendor=0x2E8A, idProduct=0x0001)
    if dev is None:
        raise ValueError('Device not found')

    while True:
        a = datetime.datetime.now()

        screen_data = get_screen_pixels()
        img = screen_data
        # width, height = img.shape[:2]
        # print("Raw image size: {}x{}".format(width, height))

        img = cv2.resize(img, (TARGET_WIDTH, TARGET_HEIGHT))

        _, jpeg_data = cv2.imencode(
            ".jpg",
            img,
            [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY]
        )
        pic = jpeg_data.tobytes()  # 转换为 bytes 对象
        size = len(pic)

        control_buffer = create_ep1_control_buffer(x, y, x + hor_res - 1, y + ver_res - 1, size)
        dev.ctrl_transfer(TYPE_VENDOR | EP_DIR_OUT, REQ_EP1_OUT, 0, 0, control_buffer)

        dev.write(EP1_OUT_ADDR, pic)
        b = datetime.datetime.now()
        c = b - a
        fps = 1000000 / c.microseconds
        print("{:.2f} fps".format(fps))
        # time.sleep(0.01)


if __name__ == "__main__":
    main()
