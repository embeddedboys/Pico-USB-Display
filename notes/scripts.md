# 用户空间工具（scripts/）

固件烧好之后，不必加载内核驱动就能验证全部功能：这些脚本直接用 pyusb 跟设备
说话，走的正是驱动使用的那套协议。调试显示链路、量测带宽、验证解码器时，
这条路比反复 `insmod`/`rmmod` 快得多。

## 依赖

```bash
pip install pyusb pillow          # 最少需要这两个
pip install numpy                 # 可选，加速 RGB565 打包
sudo apt install ffmpeg           # 只放视频/录屏时需要
pip install lz4                   # 只在测 LZ4 解码器时需要
```

选型说明（本项目实测）：

| 库 | 体积 | 用途 |
| --- | --- | --- |
| `pyusb` | 小 | **必需**，唯一的硬依赖 |
| `Pillow` | ≈3 MB | **推荐**的图片解码（JPEG/PNG/BMP），替代 `opencv-python` |
| `numpy` | ≈20 MB | 只用于把 RGB565 打包从 26 ms 降到 2 ms（整帧）；**不是必需** |
| `ffmpeg` CLI | 系统包 | 视频解码与录屏，走 rawvideo 管道，无需 Python 绑定 |

用 Pillow 而不是 `opencv-python`：功能重合，体积差约 20 倍。`numpy` 缺席时
`pud_usb` 会回退到纯 Python 打包路径（实测 480×320 用 26 ms，结果字节一致）。

## 免 root

仓库自带的 udev 规则让设备对普通用户可写：

```bash
sudo cp 60-pico-usb-display.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

> **文件名里的 `60-` 不是随便取的**：udev 把 `/etc` 与 `/usr/lib` 的规则按字典序
> 合并执行，`/usr/lib/udev/rules.d/50-udev-default.rules` 会把 usb 设备设成
> `MODE="0664"`。规则若叫 `50-...` 会排在它**之前**而被覆盖，等于没生效
> （表现为 pyusb 报 `[Errno 13] Access denied`）。

## 共享模块 `pud_usb.py`

所有脚本都从它取协议常量、QOI 编码器、坐标转换与设备句柄，**全仓库只有这一个
QOI 编码器**（与固件/驱动共用的 `rgb565_qoi.c` 逐字节一致，自检见下）。

```bash
python3 scripts/pud_usb.py      # 自检：对照 C 库参考向量校验编码器
```

主要接口：

| 接口 | 作用 |
| --- | --- |
| `open_device()` | 查找并 claim 接口，失败时给出可操作的提示（权限 / 驱动占用） |
| `Display.send_rgb565(px, w, h, x, y)` | 发一个矩形，必要时按驱动规则分带 |
| `Display.send_raw(payload, ...)` | 发已压缩的数据（非 QOI 编码器或测试用） |
| `Display.get_sn()` | 读 8 字节板子唯一 ID |
| `load_image(path, w, h, fit)` | 解码图片为 RGB888（Pillow→cv2 依次尝试） |
| `video_frames(path, w, h, fps, fit)` | ffmpeg 管道逐帧产出 RGB888 |
| `ffmpeg_frames(cmd, w, h)` | 通用 ffmpeg 取帧（录屏脚本复用它） |
| `qoi_encode(px)` / `rgb888_to_rgb565()` / `crop_rgb565()` | 编码与像素工具 |

`send_*` 会校验矩形是否越出面板（超界直接报错，而不是发出一个被硬件裁掉的窗口）。

## 脚本一览

| 脚本 | 作用 | 依赖 |
| --- | --- | --- |
| `img_viewer.py` | 显示一张图片 | Pillow 或 cv2 |
| `video_player.py` | 播放视频（不落盘） | ffmpeg |
| `fps_bench.py` | 全刷/局刷 FPS 基准 | numpy |
| `ep1_out_speed_test.py` | EP1 纯带宽扫描 | 无 |
| `ep2_protocal_test.py` | EP2 查询通道测试 | 无 |
| `lz4_img_viewer.py` | 用 LZ4 解码器显示图片（需 `DECODER_TYPE=2`） | lz4 |
| `xorg_desktop_share.py` | 把 X11 桌面镜像到面板（只发变化区域） | ffmpeg + X11 |

典型用法：

```bash
python3 scripts/img_viewer.py assets/xfce.jpg
python3 scripts/img_viewer.py --width 160 --height 120 --x 100 --y 60 -r 50 assets/bootlogo.jpg
python3 scripts/video_player.py --fps 8 --frames 200 ~/Videos/jazz.mp4
python3 scripts/fps_bench.py --frames 200
python3 scripts/fps_bench.py --dry-run          # 不接设备也能看各用例载荷大小
python3 scripts/ep1_out_speed_test.py
python3 scripts/xorg_desktop_share.py --fps 15 --stats
```

## 实测数据（供对照）

在同一块板子、`PIO_USE_DMA=1` 的固件上：

**EP1 带宽**（`ep1_out_speed_test.py`）—— 从 11 KB 到 444 KB 平坦在 **1.01–1.05 MB/s**，
说明是带宽受限而非开销受限：

| 窗口高 | 字节/帧 | MB/s | fps |
| --- | --- | --- | --- |
| 8 | 11115 | 1.010 | 90.9 |
| 64 | 88896 | 1.046 | 11.8 |
| 320 | 444352 | 1.043 | 2.3 |

**全刷/局刷**（`fps_bench.py`）：

| 用例 | 载荷 | 帧率 | 瓶颈 |
| --- | --- | --- | --- |
| partial 64×64 | 3.2 KB | ~320 fps | USB |
| partial 128×64 | 6.1 KB | ~163 fps | USB |
| partial 480×8 | 3.0 KB | ~312 fps | USB |
| full solid | 2.6 KB | ~27 fps | 面板写入 |
| full photo | ~122 KB | ~8 fps | USB |
| full noise | ~445 KB | ~2.3 fps | USB |

结论：**局刷由 USB 带宽决定**（`fps ≈ 1.05e6 / 每帧字节数`）；**全刷由面板写入决定**
（153600 像素固定约 36 ms ≈ 4.3 Mpx/s），只有单帧压缩后超过约 40 KB 才转为 USB 受限。

## 固件侧配合的注意事项

- 默认 `DECODER_TYPE=3`（QOI）。图片/视频脚本都按 QOI 发；用 LZ4 脚本前必须先把
  固件改成 `DECODER_TYPE=2`，否则数据会被 QOI 解码器丢弃（不会崩，但屏幕不动）。
- 固件的 EP2 查询路径打了 UART 日志（`usb_hexdump` + `USB_LOG_WRN`），
  实测每次查询约 **9.6 ms** —— 需要频繁查询时先去掉这些打印。
- `lz4_drawimg()` 每帧 `malloc`/`free` 约 307 KB 工作区、每帧 3 行 `printf`
  （115200 波特下约 10 ms），且是整帧解码。用 LZ4 前值得先修这三点。

## 用 Xvfb 验证录屏脚本

`xorg_desktop_share.py` 需要 X11；Wayland 会话下 `x11grab` 打不开显示，脚本会以
非零码退出并说明原因。没有物理 X 时可用虚拟显示验证：

```bash
Xvfb :99 -screen 0 1280x720x24 &
DISPLAY=:99 xsetroot -solid steelblue
DISPLAY=:99 xclock -update 1 -geometry 260x260+900+60 &
python3 scripts/xorg_desktop_share.py --display :99 --fps 15 --stats
```

验证结果：只发出时钟区域变化的小块（如 `41x56 @ (359,54)`、`69x38 @ (358,75)`），
而不是整屏，说明脏区检测按预期工作。
