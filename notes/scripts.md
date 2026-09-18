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
| `Display.query_caps()` | 问设备能力（`PUD_CMD_GET_CAPS`）：`frame_max` / `decoder_type` |
| `load_image(path, w, h, fit)` | 解码图片为 RGB888（Pillow→cv2 依次尝试） |
| `video_frames(path, w, h, fps, fit)` | ffmpeg 管道逐帧产出 RGB888 |
| `ffmpeg_frames(cmd, w, h)` | 通用 ffmpeg 取帧（录屏脚本复用它） |
| `qoi_encode(px)` / `rle_encode(px)` | 编码器，各自与 C 库逐字节一致（自检验证） |
| `rgb888_to_rgb565()` / `crop_rgb565()` | 像素工具 |

`send_*` 会校验矩形是否越出面板（超界直接报错，而不是发出一个被硬件裁掉的窗口），
也会按**设备上报**的 `frame_max` 校验载荷大小（`open_device()` 时问一次，见下）。

## 脚本一览

| 脚本 | 作用 | 依赖 |
| --- | --- | --- |
| `img_viewer.py` | 显示一张图片（`--codec qoi/rle/lz4` 指定设备构型） | Pillow 或 cv2 |
| `video_player.py` | 播放视频（不落盘） | ffmpeg |
| `fps_bench.py` | 全刷/局刷 FPS 基准 | numpy |
| `ep1_out_speed_test.py` | EP1 纯带宽扫描 | 无 |
| `ep2_protocal_test.py` | EP2 查询通道测试 | 无 |
| `touch_test.py` | EP4 触摸上报测试（`--mode push/poll`、`--calibrate`） | 无 |
| `lz4_img_viewer.py` | 同上，LZ4 专用名字（需 `DECODER_TYPE=2`）；**分带**由 `pud_usb` 负责 | lz4 |
| `codec_compare.py` | QOI / RLE / LZ4 同内容端到端对比（需按构型分次烧写） | numpy |
| `desktop_codecs.py` | **桌面负载**：按“桌面会脏的矩形”比较编解码器 | numpy |
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

## 触摸（`touch_test.py`）

```bash
python3 scripts/touch_test.py                    # 推送模式，听 10 s，边摸边打印
python3 scripts/touch_test.py --mode poll        # REQ_EP4_IN 轮询模型
python3 scripts/touch_test.py --calibrate        # 另给触摸包围盒，判断轴序/反向
```

设备主动推送 8 字节报告（布局见 [usb-protocol.md](usb-protocol.md)），所以主机只是
不断读 EP4；`--mode poll` 用 `REQ_EP4_IN` 每次取一帧，用来对比两种模型。
汇总会打印报告数、x/y 范围、报告间隔中位数、sequence 跳变（推送模式下即被合并/漏掉的
样本数）。空闲时读会超时，这是正常的（设备不发声）。

`pud_usb.py` 里有 `parse_touch_report()`、`Display.touch_request()`、
`Display.read_touch()`，新脚本请复用。

## 桌面负载下的编解码器比较（`desktop_codecs.py`）

这个项目面向桌面，主负载是**局部刷新**，整屏照片测试不代表它，所以单独有一份负载：

```bash
python3 scripts/desktop_codecs.py                                          # 合成桌面，只算载荷
python3 scripts/desktop_codecs.py --image shot.png                         # 用真实桌面截图
python3 scripts/desktop_codecs.py --image shot.png --device --codec lz4     # 再上板测时间
```

按“桌面会脏的矩形”逐个比较（默认合成一帧 480×320 桌面；给了 `--image` 就用真实截图）。
`--device` 会把每个 band **先编码好**再计时（复用 `fps_bench.measure()`），并把编码耗时
单独打成一列 —— 手写 Python 的 QOI/RLE 编码器整帧要几十毫秒，混进计时就会得出错误结论
（见 [decoders.md](decoders.md) 的“测量纠正”）。
**内容越真实越好**：合成桌面偏平坦，会高估 LZ4 的压缩率。真实截图怎么取（板上
GNOME/Wayland 的坑）、两套真实内容（整屏缩放到 480×320 / 4K 里 1:1 裁 480×320）的完整
实测与结论见 [decoders.md](decoders.md)。

> **小矩形连发会把板子的 USB 打挂**（未定因，见 [todo.md](todo.md)）：脚本默认
> `--gap-ms 3`，别调成 0 跑长循环。

## 固件侧配合的注意事项

- 默认 `DECODER_TYPE=3`（QOI）。图片/视频脚本都按 QOI 发；换成 `2`（LZ4）才能用
  `lz4_img_viewer.py`，换成 `0`/`1`（tjpgd / JPEGDEC）才能收 JPEG —— 发错格式不会崩，
  但屏幕上不动。**JPEG 只能整屏发（`x = y = 0`）**：JPEGDEC 在 `x != 0` 时会卡死显示，
  见 [decoders.md](decoders.md)。仓库里没有发 JPEG 的脚本，测试直接用
  `Display.send_raw(jpeg_bytes, 0, 0, 479, 319)`。**LZ4 必须分带**（`band_pixels`），
  整帧 block 解不了。
- `open_device()` 会顺带发一次 `PUD_CMD_GET_CAPS`，把设备的上限落到 `disp.frame_max` 与
  `disp.band_pixels`，`send_rgb565()` 按它分带；设备不认这条命令（老固件）时保留本机默认
  值（65535 B / 21839 px），所以同一份脚本能同时伺候 RP2350（64 KB）与 RP2040（32 KB）。
- 固件的 EP2 查询路径打了 UART 日志（`usb_hexdump` + `USB_LOG_WRN`），
  实测每次查询约 **9.6 ms** —— 需要频繁查询时先去掉这些打印。
- LZ4 已经重写：静态 band 缓冲、无 `printf`、每个传输一个 band（见
  [decoders.md](decoders.md)）。要发 LZ4 就用 `--codec lz4` / `codec="lz4"`，
  **不要试图整帧发**——设备会丢弃并让 `g_decoder_stat_lz4_oversize` 加一。

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

---

# C 工具 `tools/pudcodec`

上面这些是**发给设备**的脚本；`tools/pudcodec` 是**离线的资产转换器**：把图片/帧序列
压成设备能解的码流，或者反过来把码流还原成图片。它把上游 `rgb565-rle` / `rgb565-qoi`
仓库里那六个体积相近的小工具（`img2rle`、`video2rle`、`rle2img` 和各自的 qoi 版）
合成一个：**编解码类型在运行时用 `--codec` 指定**。

```bash
cmake -S tools -B tools/build && cmake --build tools/build -j     # 主机侧构建，与固件无关
```

`stb_image.h` / `stb_image_write.h` 已 vendor 在 `tools/` 里，**配置时不需要联网**
（上游是配置时下载的）。构建产物在 `tools/build/`（已 gitignore），不进固件镜像。

## 用法

```bash
pudcodec --codec <qoi|rle|lz4|jpeg> img2s   [options] <image>     # 图片 -> 码流
pudcodec --codec <qoi|rle|lz4|jpeg> s2img   [options] <stream>    # 码流 -> 图片
pudcodec --codec <qoi|rle|lz4|jpeg> video2s [options] <frames...> # 帧序列 -> 容器
```

| 选项 | 说明 |
| --- | --- |
| `--codec` | `qoi` / `rle` / `lz4` / `jpeg`；`auto` 表示从 `.h` 里的 `_CODEC` 标签取 |
| `-o` | 输出路径（默认 `<输入>.<codec>.h/.bin`，`s2img` 默认 `<输入>.png`） |
| `-t` | `img2s`/`video2s`：`h`（C 头，默认）或 `bin`；`s2img`：`png`/`jpg`/`bmp`/`tga` |
| `-n` | C 数组名 / 基名（默认从输出文件名推，取到第一个 `.` 为止） |
| `-w` `-h` | `img2s`/`video2s` 是缩放目标；`s2img` 读 `.bin` 时**必须给**（码流里没有尺寸，JPEG 除外） |
| `-q` | JPEG 质量，默认 95 |
| `--band` | LZ4 only：每个 block 的行数（默认取"装得下且能整除高度"的最大值） |
| `--raw` | `video2s` 的输入是拼接好的裸 RGB565 帧 |

编解码对应关系：`jpeg` 覆盖设备侧的 `DECODER_TYPE` 0 和 1（两种 JPEG 解码器吃同一份
码流），`lz4` 是 2、`qoi` 是 3、`rle` 是 4。工具会把 `decoder_type` 打在摘要里，
省得回头翻文档。

**LZ4 输出的是 band 容器**（`[count][offsets][blocks]`，每 band 一个 block），不是单个
整帧 block：LZ4 block 不能分块解码，设备一次只持有一个 band（见
[decoders.md](decoders.md) 与 `AGENTS.md` 第 9 条）。`--band` 不指定时取"装得下设备
band 缓冲（43678 B）且能整除图像高度"的最大行数，这样 band 高度能从
`block 数 / 图像高度` 推回来，`s2img` 与固件的开机 logo 才能重建。
`video2s --codec lz4` 对每一帧都这样分带，容器是**扁平的**（帧优先，一帧内自上而下）。

开机 logo 的 LZ4 分支就是这么生成的（`assets/bootlogo.jpg` 经无损 PNG 再转裸 RGB565）：

```bash
python3 -c "from PIL import Image; Image.open('assets/bootlogo.jpg').save('/tmp/logo.png')"
python3 -c "
import sys; sys.path.insert(0, 'scripts'); import pud_usb as P
open('/tmp/logo.raw','wb').write(P.rgb888_to_rgb565(P.load_image('/tmp/logo.png',480,320,fit=False),480,320))"
tools/build/pudcodec --codec lz4 video2s --raw /tmp/logo.raw -w 480 -h 40 -t bin -o /tmp/logo.lz4.bin
```

再把这串字节替换进 `include/bootlogo.h` 的 `#elif DECODER_TYPE == 2` 段
（`check_pudcodec.py` 会验证这个分支与工具的产物逐字节相同）。

## 与固件/脚本的一致性（2026-09 实测）

`scripts/check_pudcodec.py` 把这条路径与 `pud_usb.py` 对拍，**不需要设备**：

```bash
python3 scripts/check_pudcodec.py     # 全部通过才返回 0
```

| 检查 | 结果 |
| --- | --- |
| `img2s --codec qoi/rle` vs `pud_usb.qoi_encode/rle_encode`（PNG 源） | **逐字节相同**（photo/noise/gradient 三份，最大 444298 B） |
| `video2s --raw` 的容器：`[count][offsets[count+1]][data]`、帧数据 | 结构正确，第 0 帧与 Python 编码器逐字节相同 |
| `s2img` 往返 | QOI/RLE/LZ4 都是 153600/153600 像素完全相同 |
| `.h` 的 `_CODEC` / `_WIDTH` / `_HEIGHT` / `_SIZE` / 帧表 | 正确；`--codec auto` 能据此自动解码 |
| JPEG 往返 | 最大 10 LSB、平均 0.20 LSB（有损，属正常） |
| `img2s --codec lz4` 的 band 容器 | 结构正确、每 band 都装得下 43678 B、`s2img` 往返像素精确 |
| **`include/bootlogo.h` 的 QOI / RLE / LZ4 分支** | 用同一张图重压，**逐字节相同**（29652 / 49485 / 17585 B） |

两条**已知的不一致**（都是用压缩工具时要知道的）：

- **JPEG 源图两条路径不逐字节相同**：`stb_image` 与 Pillow/libjpeg 解 JPEG 的取证
  （IDCT 舍入）不同，同一张 `bootlogo.jpg` 一个出 49485 B、一个出 49494 B。要比字节
  就用无损源（PNG）或 `--raw` 喂同一份 RGB565；差异 ≤1 LSB，屏上看不出来。
- **LZ4 码流不跨版本逐字节一致**：工具 vendor 的 liblz4 是 1.10.0，板子上 python-lz4
  4.4.5 带的是 1.9.x，同一条 band **8 条里有 3 条**压缩结果不同（都合法）。设备只解压，
  `LZ4_decompress_safe` 与版本无关；两条来源的码流**都在板上验证过像素精确**
  （工具生成的 bootlogo 资产、`pud_usb.lz4_encode` 发的帧）。要比字节就固定同一个
  liblz4 版本。

> RGB565 的打包用**截断**（`r >> 3`）而不是四舍五入，跟 `pud_usb.py` 保持一致 ——
> 这是两条主机路径能逐字节对拍的前提。
