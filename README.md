Pico USB Display
============================

[中文](./README.md) | [English](./README.en.md)

您是否考虑过使用 Pico 开发板作为显示设备？
如果没有，您可能会对这个项目感兴趣。

<!-- (put a preview video here) -->

https://github.com/user-attachments/assets/9541297a-1a50-4cca-b3d9-013d84244e89

https://github.com/user-attachments/assets/7fa16b4b-5ea4-4122-a23e-d53f6be5380d

![desktop_xfce](./assets/xfce.jpg)

## 特性

- 🚀 易于使用（一块 Pico 开发板和 TFT 显示器，加上一些杜邦线）
- 📦 支持多平台（Linux、Windows、macOS）

## 如何使用？

硬件需求

- Raspberry Pi Pico
- 1 个 SPI 或 I8080 TFT 显示器，[这里]() 是兼容驱动列表

### 设置您的 Pico 开发板

前往 GitHub Release 页面下载预编译的固件 uf2 文件。

另外，如果您想自己编译固件（假设您使用的是 Ubuntu 机器）

#### 1. 安装 [Pico SDK](https://github.com/raspberrypi/pico-sdk)

```bash
git clone https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk
git submodule update --init
```

#### 2. 安装 CMake（至少 3.13 版本）、python 3、本地编译器和 GCC 交叉编译器

```bash
sudo apt install cmake python3 build-essential gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib ninja-build
```

#### 3. 克隆此仓库

```bash
git clone https://github.com/embeddedboys/Pico-USB-Display.git
cd Pico-USB-Display
```

#### 4. 选择您需要的配置，打开 `CMakeLists.txt` 并：

==您可以在配置文件中找到引脚定义。==

```cmake
# **********  选择合适的配置文件  **********
include(${PICO_DISPLAY_LIB_CONFIG_PATH}/pico_dm_qd3503728.cmake)
# include(${PICO_DISPLAY_LIB_CONFIG_PATH}/pico_dm_qd3503728_8bit.cmake)
# include(${PICO_DISPLAY_LIB_CONFIG_PATH}/pico_dm_yt350s006.cmake)
# include(${PICO_DISPLAY_LIB_CONFIG_PATH}/generic-ili9341.cmake)
# include(${PICO_DISPLAY_LIB_CONFIG_PATH}/generic-st7789v.cmake)
```

#### 5. 然后构建固件
```bash
mkdir -p build && cd build
cmake .. -G Ninja
ninja
```
```text
[82/84] Linking CXX executable pico-usb-display.elf
Memory region         Used Size  Region Size  %age Used
           FLASH:      256700 B         2 MB     12.24%
             RAM:      163604 B       256 KB     62.41%
       SCRATCH_X:          2 KB         4 KB     50.00%
       SCRATCH_Y:          2 KB         4 KB     50.00%
[84/84] Print target size info
      text       data        bss      total filename
     98328     158372      32792     289492 pico-usb-display.elf
```

您可以在 `build` 目录中找到固件。

#### 6. 将 udev 规则复制到 `/etc/udev/rules.d/`

如果您不想使用 root 权限访问 USB 设备，则需要正确配置 udev 规则。
```bash
sudo cp 50-pico-usb-display.rules /etc/udev/rules.d/

# 然后重新加载 udev 规则
sudo udevadm control --reload-rules && sudo udevadm trigger
```

#### 7. 将固件烧录到您的 Pico 开发板

Pico 提供了一个引导加载程序，可以轻松地将固件烧录到 Pico 开发板。在插入 Pico 开发板时按住 `BOOTSEL` 按钮，将会挂载一个名为 `RPI-RP2` 或 `RP2350` 的驱动器。然后将 `pico-usb-display.uf2` 文件复制到该驱动器，固件将被烧录到 Pico 开发板。

固件烧录完成后，您将看到屏幕上显示一些内容。这是一个示例：

![boot](./assets/bootlogo.jpg)

### 显示图片和视频

如果您不想进行深度定制，那么 Python 脚本是使用它的最简单方法，但 DRM 驱动或其他方法（未来）会更高效。

#### Python 脚本

创建一个 python3 虚拟环境并安装依赖项

```bash
python3 -m venv .venv
source .venv/bin/activate

pip install pyusb opencv-python numpy
```

如果您想在 pico 显示器上显示图片：
```bash
# 用法: ./scripts/img_viewer.py [xres] [yres] <file.jpg>

./scripts/img_viewer.py 480 320 ~/Pictures/artplayer_19_21.png
```

或者您想在 pico 显示器上播放视频：
```bash
# 用法: ./scripts/video_player.py [xres] [yres] [quality|1-100] <video.mp4>

./scripts/video_player.py 480 320 50 ~/Videos/jazz_15fps.mp4
```

您可能还想知道如何将视频设置为 15fps：
```bash
ffmpeg -i ./jazz.mp4 -vf "fps=15" -c:v libx264 -preset fast -crf 23 -c:a copy ./jazz_15fps.mp4
```

#### 内核驱动

```bash
git clone https://github.com/embeddedboys/PUD-kernel-drivers
cd PUD-kernel-drivers

make
sudo insmod pud.ko
```

然后您可以使用以下命令轻松播放视频：

```bash
ffplay ./jazz.mp4
```

## 本项目使用的开源软件

- [FreeRTOS-Kernel](https://github.com/FreeRTOS/FreeRTOS-Kernel)
- [bitbank2/JPEGENC](https://github.com/bitbank2/JPEGENC)
- [bitbank2/JPEGDEC](https://github.com/bitbank2/JPEGDEC)
- [dgatf/usb_library_rp2040](https://github.com/dgatf/usb_library_rp2040)
- [cherry-embedded/CherryUSB](https://github.com/cherry-embedded/CherryUSB)
- [Bodmer/TJpg_Decoder](https://github.com/Bodmer/TJpg_Decoder)
- [embeddedboys/pico_dm_qd3503728_freertos](https://github.com/embeddedboys/pico_dm_qd3503728_freertos)

## 链接

- [PUD-kernel-drivers - Pico USB Display 的 DRM 驱动程序](https://github.com/embeddedboys/PUD-kernel-drivers)

- [A USB display device based on Raspberry Pi Pico & Pico_DM_QD3503728.](https://github.com/embeddedboys/pico_dm_qd3503728_udd)