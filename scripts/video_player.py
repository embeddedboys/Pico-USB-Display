#!/usr/bin/env python3

#
# Copyright (c) 2020 2024 Daniel Gorbea
#
# Copyright (c) 2020 Raspberry Pi (Trading) Ltd. author of https://github.com/raspberrypi/pico-examples/tree/master/usb
#
# Copyright (c) 2025 embeddedboys developers
#
# SPDX-License-Identifier: BSD-3-Clause
#

'''
## Install python requirements by running
$ python -m venv .venv
$ source .venv/bin/activate
$ pip install opencv-python pyusb

## limit video fps to 15 is recommended for this demo
$ ffmpeg -i ./jazz.mp4 -vf "fps=15" -c:v libx264 -preset fast -crf 23 -c:a copy /tmp/output.mp4

## run the script from project root directory
$ sudo ./scripts/video_player.py 480 320 50 /tmp/output.mp4
'''

usage = "Usage: sudo {} [xres] [yres] [quality|1-100] <video.mp4>"

import os
import sys
import cv2
import time

import usb.core
import usb.util
import datetime

# Directory to store extracted frames
FRAMES_DIR = "/tmp/pud_frames"

# Maximum length of binary data to be transmitted
MAX_LENGTH = 40000

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

def extract_frames_from_video(video_path, output_dir, xres=480, yres=320, quality=25) -> int:
    """Extracts frames from a video file and saves them as JPG files."""
    fps = 0
    try:
        os.makedirs(output_dir, exist_ok=True)
        # cleanup previously extracted frames
        os.system(f"sudo rm -rf {output_dir}/*")
        video_capture = cv2.VideoCapture(video_path)
        frame_count = 0

        fps = int(video_capture.get(cv2.CAP_PROP_FPS))
        print("video fps is :", fps)

        # Iterate through the frames
        print("resize frame to {}x{}".format(xres, yres))

        print("Extracting frames from input file...")
        while True:
            success, frame = video_capture.read()
            if not success:
                break

            # Resize the frame to the target resolution
            frame = cv2.resize(frame, (xres, yres))

            frame_file = os.path.join(output_dir, f"frame_{frame_count:08}.jpg")
            cv2.imwrite(frame_file, frame, [
                cv2.IMWRITE_JPEG_QUALITY, quality,
                cv2.IMWRITE_JPEG_OPTIMIZE, 1,
            ])
            frame_count += 1

        video_capture.release()
        print(f"Extracted {frame_count} frames from {video_path}.")
    except Exception as e:
        print(f"Error extracting frames from {video_path}: {e}")
    return fps

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

def main():
    xres = 480
    yres = 320
    quality = 25    # JPEG quality for compression (1 to 100, higher is better quality)
    video_file = None
    if len(sys.argv) < 2:
        print(usage.format(sys.argv[0]))
        sys.exit(1)

    # command line parsing
    if len(sys.argv) == 3:
        xres = int(sys.argv[1])
    elif len(sys.argv) == 4:
        xres = int(sys.argv[1])
        yres = int(sys.argv[2])
    elif len(sys.argv) == 5:
        xres = int(sys.argv[1])
        yres = int(sys.argv[2])
        quality = int(sys.argv[3])

    video_file = sys.argv[-1]
    # Validate video file existence
    if not os.path.isfile(video_file):
        print(f"Error: The file '{video_file}' does not exist.")
        return

    # Extract frames from the provided video file
    video_fps = extract_frames_from_video(video_file, FRAMES_DIR, xres, yres, quality)

    # Initialize the list to store binaries
    media_binaries = []

    # Iterate through the extracted frames
    for file_name in sorted(os.listdir(FRAMES_DIR)):
        file_path = os.path.join(FRAMES_DIR, file_name)

        if file_name.endswith(".jpg"):
            try:
                # Read the file in binary mode
                with open(file_path, "rb") as file:
                    binary_data = file.read()

                # Check the length of the binary data
                if len(binary_data) <= MAX_LENGTH:
                    media_binaries.append(binary_data)
                else:
                    print(f"Skipping {file_name}: Binary size exceeds {MAX_LENGTH} bytes.")
            except Exception as e:
                print(f"Error reading {file_name}: {e}")

    # Output the results
    print(f"Number of frames added to the list: {len(media_binaries)}")

    dev = usb.core.find(idVendor=0x2E8A, idProduct=0x0001)
    if dev is None:
        raise ValueError('Device not found')

    i = 0
    while True:
        if i >= len(media_binaries):
            i = 0
        pic = media_binaries[i]

        if len(pic) % 2 != 0:
            pic = pic + b'\xff'
        size = len(pic)

        # make a control transfer
        control_buffer = create_ep1_control_buffer(x, y, x + xres - 1, y + yres - 1, size)
        dev.ctrl_transfer(TYPE_VENDOR | EP_DIR_OUT, REQ_EP1_OUT, 0, 0, control_buffer)
        start = datetime.datetime.now()
        dev.write(EP1_OUT_ADDR, pic)
        end = datetime.datetime.now()
        elapsed = end - start
        print("frame time : {:.2f} ms".format(elapsed.microseconds / 1000))

        # framerate control
        delay_ms = 1000 / video_fps
        # start = time.perf_counter()
        # while (time.perf_counter() - start) < delay_ms / 1000.0:
        #     pass
        time.sleep(delay_ms / 1000.0)

        # move to next frame and loop
        i = (i + 1) % len(media_binaries)


if __name__ == "__main__":
    main()
