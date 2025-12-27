#!/usr/bin/env python3

import datetime
import usb.core
import usb.util

dev = usb.core.find(idVendor=0x2E8A, idProduct=0x0001)
if dev is None:
    raise ValueError('Device not found')

EP_DIR_OUT = 0x00
EP_DIR_IN = 0X80
TYPE_VENDOR = 0X40

REQ_EP0_OUT = 0X00
REQ_EP0_IN = 0X01
REQ_EP1_OUT = 0X02
REQ_EP2_IN = 0X03

size = 30000
repeat = 1
kBs = 0

buffer = bytearray((i % 10) + 0x30 for i in range(size))

x = 400
y = 300
xres = 480
yres = 320
print(x, y, hex(x), hex(y))

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

control_buffer = create_ep1_control_buffer(x, y, x + xres - 1, y + yres - 1, size)
print(control_buffer, len(buffer))

for i in range(repeat):
    dev.ctrl_transfer(TYPE_VENDOR | EP_DIR_OUT, REQ_EP1_OUT, 0, 0, control_buffer)
    a = datetime.datetime.now()
    dev.write(0x01, buffer)
    b = datetime.datetime.now()
    elapsed = (b - a).total_seconds()
    kBs += (size / 1024) / elapsed

print("Request REQ_EP1_OUT. Size: %u bytes. Speed: %.2f kBs" % (size, kBs / repeat))
