#!/usr/bin/env python3

import os
import sys
import time

import usb.core
import usb.util
import datetime

EP_DIR_OUT = 0x00
EP_DIR_IN = 0X80
TYPE_VENDOR = 0X40

EP1_OUT_ADDR = (EP_DIR_OUT | 0x01)
EP2_IN_ADDR = (EP_DIR_IN | 0x02)

REQ_EP0_OUT = 0X00
REQ_EP0_IN = 0X01
REQ_EP1_OUT = 0X02
REQ_EP2_IN = 0X03

def create_ctrl_buf(cmd, len):
	return [cmd & 0xff, cmd >> 8, len & 0xff, len >> 8]

dev = usb.core.find(idVendor=0x2E8A, idProduct=0x0001)
if dev is None:
    raise ValueError('Device not found')

ctrl_buf = create_ctrl_buf(0x01, 8);
print(ctrl_buf, len(ctrl_buf))

dev.ctrl_transfer(TYPE_VENDOR | EP_DIR_OUT, REQ_EP2_IN, 0, 0, ctrl_buf)
data = dev.read(EP2_IN_ADDR, 8);
print(data)
