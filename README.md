Pico USB Display
============================

Have you ever considered using your Pico board as a display device?
If not, you might be interested in this project.

(put a preview video here)

## Features

- 🚀 Easy to use (A Pico board and TFT Display with some jumper wires)
- 📦 Supports multiple platforms (Linux, Windows, macOS)

## How to?

Hardware requirements

- Raspberry Pi Pico
- 1 x SPI or I8080 TFT display, [here]() a list of compatible driver

### Setup your Pico board

Go to the Github Release page and download the prebuilt firmware uf2 file.

Also, If you want to compile the firmware yourself (Assuming you are using a Ubuntu machine)

1. Install the [Pico SDK](https://github.com/raspberrypi/pico-sdk)

```bash
git clone https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk
git submodule update --init
```

2. Install CMake (at least version 3.13), python 3, a native compiler, and a GCC cross compiler

```bash
sudo apt install cmake python3 build-essential gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
```

#### Flash the firmware to your Pico board

Pico has provided a bootloader that can easily flash the firmware to the Pico board. Hold the `BOOTSEL` button while plugging in the Pico board, and a drive named `RPI-RP2` or `RP2350` will be mounted. Then you can copy the `uf2` file to the drive and the firmware will be flashed to the Pico board.