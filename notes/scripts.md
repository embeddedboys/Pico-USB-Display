# 用户空间工具（`tools/`）与验证脚本（`tests/`）

> 固件烧好后**不必加载内核驱动**就能验证全部功能：这些脚本用 pyusb 直连设备，走的正是驱动
> 使用的那套协议 —— 调试显示链路、量带宽、验证解码器时比反复 `insmod`/`rmmod` 快得多。

## TL;DR

- 依赖只需 `pyusb`；`Pillow` 替代 `opencv-python`（体积差约 20 倍），`numpy`/`ffmpeg`/`lz4` 可选。
- 免 root 靠仓库根 `60-pico-usb-display.rules`；**文件名里的 `60-` 不能退回 `50-`**。
- **全仓库只有一份编码器**（`tools/pud_usb.py` 的 `ENCODERS`），新脚本必须复用它。
- 量吞吐前先看拓扑：**全速设备别挂在 hub 后面**（经 hub 0.833 MB/s 对直插根口 1.132 MB/s，+36%）。
- **给设备计时必须把编码放在循环外**；绝对 MB/s 是会话属性，只有比值能搬。

## 依赖

```bash
pip install pyusb pillow          # 最少需要这两个
pip install numpy                 # 可选，加速 RGB565 打包
sudo apt install ffmpeg           # 只放视频/录屏时需要
pip install lz4                   # 只在测 LZ4 解码器时需要
```

| 库 | 体积 | 用途 |
| --- | --- | --- |
| `pyusb` | 小 | **必需**，唯一的硬依赖 |
| `Pillow` | ≈3 MB | **推荐**的图片解码（JPEG/PNG/BMP），替代 `opencv-python` |
| `numpy` | ≈20 MB | 只用于把 RGB565 打包从 26 ms 降到 2 ms（整帧）；**不是必需** |
| `ffmpeg` CLI | 系统包 | 视频解码与录屏，走 rawvideo 管道，无需 Python 绑定 |

用 Pillow 而不是 `opencv-python`：功能重合，体积差约 20 倍。`numpy` 缺席时 `pud_usb` 会回退
到纯 Python 打包路径（实测 480×320 用 26 ms，结果字节一致）。

## 免 root

```bash
sudo cp 60-pico-usb-display.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

> **文件名里的 `60-` 不是随便取的**：udev 把 `/etc` 与 `/usr/lib` 的规则按字典序合并执行，
> `/usr/lib/udev/rules.d/50-udev-default.rules` 会把 usb 设备设成 `MODE="0664"`。规则若叫
> `50-...` 会排在它**之前**而被覆盖，等于没生效（表现为 pyusb 报 `[Errno 13] Access denied`）。

## 共享模块 `pud_usb.py`

所有脚本都从它取协议常量、编码器、坐标转换与设备句柄，**全仓库只有这一个 QOI/RLE 编码器**
（与固件/驱动共用的 `rgb565_qoi.c` 逐字节一致，自检见下）。

```bash
python3 tools/pud_usb.py      # 自检：对照 C 库参考向量校验编码器
```

| 接口 | 作用 |
| --- | --- |
| `open_device()` | 查找并 claim 接口，失败时给出可操作的提示（权限 / 驱动占用） |
| `Display.send_rgb565(px, w, h, x, y)` | 发一个矩形，必要时按驱动规则分带 |
| `Display.send_raw(payload, ...)` | 发已压缩的数据（非 QOI 编码器或测试用） |
| `Display.get_sn()` | 读 8 字节板子唯一 ID |
| `Display.query_caps()` | 问设备能力（`PUD_CMD_GET_CAPS`）：`frame_max` / `decoder_type` / `band_pixels` |
| `Display.set_params()` / `get_params()` | 运行期参数通道（`SET_PARAM`/`GET_PARAM`） |
| `load_image(path, w, h, fit)` | 解码图片为 RGB888（Pillow→cv2 依次尝试） |
| `video_frames(path, w, h, fps, fit)` / `ffmpeg_frames()` | ffmpeg 管道逐帧产出 RGB888 |
| `qoi_encode(px)` / `rle_encode(px)` / `lz4_encode()` | 编码器，各自与 C 库逐字节一致（自检验证） |
| `rgb888_to_rgb565()` / `crop_rgb565()` | 像素工具 |

`send_*` 会校验矩形是否越出面板（超界直接报错），也会按**设备上报**的 `frame_max` 校验载荷大小
（`open_device()` 时问一次）。

## 脚本一览

**三个目录，各管一件事**：

- `tools/` 放工具与演示（发图、播放、录屏、生成 bootlogo，以及 C 写的离线转换器 `pudcodec`），
  共享库 `tools/pud_usb.py` 也在那里 —— 工具和测试都 import 它。
- `tests/` 放**验证脚本**：跑一遍给出对/错（读回核对、退出码非零即失败），各自
  `sys.path.insert` 到 `tools/` 才能 import `pud_usb`，所以**从仓库根目录跑**。
- `scripts/` 是**构建脚本**（`build.sh` / `lunch.sh` / `flash.sh` / `config-info.sh`），
  与设备无关，见 [build-and-flash.md](build-and-flash.md)。

| 脚本 | 作用 | 依赖 |
| --- | --- | --- |
| `tools/img_viewer.py` | 显示一张图片（`--codec qoi/rle/lz4` 指定设备构型） | Pillow 或 cv2 |
| `tools/video_player.py` | 播放视频（不落盘，`--codec` 同上） | ffmpeg |
| `tools/fps_bench.py` | 全刷/局刷 FPS 基准（QOI/RLE/LZ4 载荷） | numpy |
| `tools/jpeg_bench.py` | **整屏 JPEG** 基准（JPEG 帧由主机给字节；配 `DECODER_TYPE=1/0` 用） | Pillow（`--image` 时） |
| `tools/touch_draw.py` | 屏上触摸反馈（`--mode trace/grid/targets`） | numpy |
| `tools/codec_compare.py` | QOI / RLE / LZ4 同内容端到端对比（需按构型分次烧写） | numpy |
| `tools/desktop_codecs.py` | **桌面负载**：按"桌面会脏的矩形"比较编解码器 | numpy |
| `tools/xorg_desktop_share.py` | 把 X11 桌面镜像到面板（只发变化区域） | ffmpeg + X11 |
| `tools/mkbootlogo.py` | 从 `assets/bootlogo.png` 重新生成 `include/bootlogo.h`（四分支，落盘前自校验；`--check` 只比对） | `tools/build/pudcodec` |
| `tests/test_ep1_throughput.py` | EP1 纯带宽扫描 | 无 |
| `tests/test_ep2_query.py` | EP2 查询通道测试 | 无 |
| `tests/test_param_channel.py` | 运行期参数通道测试（调暗/调亮并读回、转朝向并核对 caps 几何、验不支持字段如实上报） | 无 |
| `tests/test_rotation_geometry.py` | 四种朝向各画一张非对称图案，看画面是否始终正立 | Pillow |
| `tests/test_touch_ep4.py` | EP4 触摸上报测试（推送/轮询、标定） | 无 |
| `tests/test_protocol_constants.py` / `test_encoder_reference.py` / `test_codec_crosscheck.py` | 协议结构体尺寸、编码器参考向量、`pudcodec` 对拍 | 无 |

> 发图统一走 `img_viewer.py`，LZ4 用 `img_viewer.py --codec lz4` —— 原来那个 `lz4_img_viewer.py`
> 与它完全重复，已删。同样需要 `DECODER_TYPE=2` 的固件，脚本会先读 caps 校验再发。

典型用法：

```bash
python3 tools/img_viewer.py assets/xfce.jpg
python3 tools/img_viewer.py --width 160 --height 120 --x 100 --y 60 -r 50 assets/bootlogo.png
python3 tools/video_player.py --fps 8 --frames 200 ~/Videos/jazz.mp4
python3 tools/fps_bench.py --frames 200
python3 tools/fps_bench.py --dry-run          # 不接设备也能看各用例载荷大小
python3 tests/test_ep1_throughput.py
python3 tools/xorg_desktop_share.py --fps 15 --stats
```

## 实测数据（供对照）

### ESP32-S3 + ILI9488（2026-10-04）

使用 `pico_dm_qd3503728_esp32s3_idf` 当前固件、QOI 解码器和 USB 全速连接实测。
设备能力为 `480x320`、`rotation=1`、`touch=True`、`proto_ver=2`、
`frame_max=32768`。测试命令：

```bash
python3 tools/fps_bench.py --frames 30 --pattern solid,gradient,photo,noise
python3 tests/ep1_out_speed_test.py --frames 60
```

EP1 吞吐在大载荷下稳定约 **0.947 MB/s**，对应结果如下：

| 窗口高 | 字节/帧 | MB/s | fps | ms/帧 |
| --- | ---: | ---: | ---: | ---: |
| 8 | 11115 | 0.937 | 84.3 | 11.87 |
| 16 | 22235 | 0.947 | 42.6 | 23.47 |
| 32 | 44497 | 0.945 | 21.2 | 47.08 |
| 64 | 88914 | 0.947 | 10.7 | 93.87 |
| 128 | 177814 | 0.947 | 5.3 | 187.67 |
| 320 | 444466 | 0.947 | 2.1 | 469.24 |

端到端 FPS：

| 用例 | 分带/窗口 | 载荷 | fps | 压缩吞吐 |
| --- | --- | ---: | ---: | ---: |
| full solid | 15 段 | 2772 B | 6.09 | 0.017 MB/s |
| full gradient | 15 段 | 32996 B | 6.04 | 0.199 MB/s |
| full photo | 15 段 | 131315 B | 6.02 | 0.790 MB/s |
| full noise | 15 段 | 444658 B | 2.13 | 0.947 MB/s |
| partial | 64x64 | 3558 B | 99.61 | 0.354 MB/s |
| partial | 128x64 | 6904 B | 97.20 | 0.671 MB/s |
| partial | 480x8 | 3270 B | 101.22 | 0.331 MB/s |

结论：大载荷的 USB 有效吞吐约 `0.95 MB/s`，全屏噪声和照片分别受 USB
带宽明显限制；当前固件的全屏分带刷新约 `6 FPS`，局部刷新约 `100 FPS`。
同一测试中纯色全屏改为单次传输可达到约 `121 FPS`，说明全屏分带和面板刷新
次数是独立的性能瓶颈。该单次传输结果仅作为协议/设备基准，不代表当前驱动的
分带刷新路径。

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
python3 tests/touch_test.py                    # 推送模式，听 10 s，边摸边打印
python3 tests/touch_test.py --mode poll        # REQ_EP4_IN 轮询模型
python3 tests/touch_test.py --calibrate        # 另给触摸包围盒，判断轴序/反向
```

设备主动推送 8 字节报告（布局见 [usb-protocol.md](usb-protocol.md)），所以主机只是
不断读 EP4；`--mode poll` 用 `REQ_EP4_IN` 每次取一帧，用来对比两种模型。
汇总会打印报告数、x/y 范围、报告间隔中位数、sequence 跳变（推送模式下即被合并/漏掉的
样本数）。空闲时读会超时，这是正常的（设备不发声）。

`pud_usb.py` 里有 `parse_touch_report()`、`Display.touch_request()`、
`Display.read_touch()`，新脚本请复用。

## 屏上触摸反馈（`touch_draw.py`）

数字只能告诉你坐标存在，画出来才能告诉你对不对。这个脚本把画布画到面板上，并在**设备
上报的坐标处**打标记，所以轴序交换、反向、旋转不跟随、贴合偏移一眼就能看出来：

```bash
python3 tools/touch_draw.py                 # trace：标记跟着手指走（默认）
python3 tools/touch_draw.py --mode grid     # 40 px 网格 + 坐标标签，按下处打点
python3 tools/touch_draw.py --mode targets  # 依次点 5 个十字靶，输出每个靶的误差
```

- `trace`/`grid` 摸就行；`grid` 会在标记旁写上设备报的 `x,y`，直接读数。
- `targets` 依次标出四角与中心，点完打印 `dx/dy`、平均误差（也就是 `x_offs/y_offs`
  能抵消的量）和 x/y 跨度（跨度不对说明轴交换或没点全）。
- 电容屏与玻璃贴合本来就有边缘偏移，**几个像素的平均误差是正常的**。

**实测**（2026-09，`--mode grid --rate 20`，10 分钟）：29 次触摸 / 193 个报告 / 29 个
release，四角落点 (0,0)、(475,0)、(475,319)、(0,314)，横拖只变 x、竖拖只变 y，
sequence 全程只跳 2 次，**板子没有挂**。同一批数据在 [usb-protocol.md](usb-protocol.md)
的 EP4 一节里也记了一份。

**默认限速**：`--rate 20`（每秒最多 20 次面板更新）。报告是 8 ms 一个（≈125 Hz），到得比
`--rate` 快的报告会被合并进下一次更新 —— 画布内容不丢，只是传输延后。`--gap-ms` 默认
**0**：2026-09 无调试器复测（3000/6000 × 32×32 与 150/300 帧桌面负载，都是 gap 0）
全部 `errors=0`、`CFSR`/`HFSR` 为 0，"小传输连发会挂"不成立，见 [todo.md](todo.md) 第 8 条。

## 桌面负载下的编解码器比较（`desktop_codecs.py`）

这个项目面向桌面，主负载是**局部刷新**，整屏照片测试不代表它，所以单独有一份负载：

```bash
python3 tools/desktop_codecs.py                                          # 合成桌面，只算载荷
python3 tools/desktop_codecs.py --image shot.png                         # 用真实桌面截图
python3 tools/desktop_codecs.py --image shot.png --device --codec lz4     # 再上板测时间
```

按“桌面会脏的矩形”逐个比较（默认合成一帧 480×320 桌面；给了 `--image` 就用真实截图）。
`--device` 会把每个 band **先编码好**再计时（复用 `fps_bench.measure()`），并把编码耗时
单独打成一列 —— 手写 Python 的 QOI/RLE 编码器整帧要几十毫秒，混进计时就会得出错误结论
（见 [decoders.md](decoders.md) 的“测量纠正”）。
**内容越真实越好**：合成桌面偏平坦，会高估 LZ4 的压缩率。真实截图怎么取（板上
GNOME/Wayland 的坑）、两套真实内容（整屏缩放到 480×320 / 4K 里 1:1 裁 480×320）的完整
实测与结论见 [decoders.md](decoders.md)。

> `--gap-ms` 默认 **0**。旧笔记说"小矩形连发会把板子的 USB 打挂"，**已否定**：
> 无调试器下 3000/6000 × 32×32 与 150/300 帧桌面负载都是 `errors=0`、`CFSR`/`HFSR` 保持 0，
> 见 [todo.md](todo.md) 第 8 条。
## 固件侧配合的注意事项

- 默认 `DECODER_TYPE=3`（QOI）。图片/视频脚本都按 QOI 发；换成 `2`（LZ4）才能用
  `img_viewer.py --codec lz4`，换成 `0`/`1`（tjpgd / JPEGDEC）才能收 JPEG —— 发错格式不会崩，
  但屏幕上不动。**JPEG 只能整屏发（`x = y = 0`）**：JPEGDEC 在 `x != 0` 时会卡死显示，见
  [decoder-architecture.md](decoder-architecture.md)。仓库里没有发 JPEG 的脚本，测试直接用
  `Display.send_raw(jpeg_bytes, 0, 0, 479, 319)`。**LZ4 必须分带**（`band_pixels`），整帧 block 解不了。
- `open_device()` 会顺带发一次 `PUD_CMD_GET_CAPS`，把设备的上限落到 `disp.frame_max` 与
  `disp.band_pixels`，`send_rgb565()` 按它分带；设备不认这条命令（老固件）时保留本机默认值
  （65535 B / 21835 px），所以同一份脚本能同时伺候 RP2350（64 KB）与 RP2040（32 KB）。
- 固件的 EP2 查询路径打了 UART 日志（`usb_hexdump` + `USB_LOG_WRN`），实测每次查询约
  **9.6 ms** —— 需要频繁查询时先去掉这些打印。
- LZ4 已经重写：静态 band 缓冲、无 `printf`、每个传输一个 band（见 [lz4.md](lz4.md)）。要发
  LZ4 就用 `--codec lz4` / `codec="lz4"`，**不要试图整帧发** —— 设备会丢弃并让
  `g_decoder_stat_lz4_oversize` 加一。

## 用 Xvfb 验证录屏脚本

`xorg_desktop_share.py` 需要 X11；Wayland 会话下 `x11grab` 打不开显示，脚本会以非零码退出并
说明原因。没有物理 X 时可用虚拟显示验证：

```bash
Xvfb :99 -screen 0 1280x720x24 &
DISPLAY=:99 xsetroot -solid steelblue
DISPLAY=:99 xclock -update 1 -geometry 260x260+900+60 &
python3 tools/xorg_desktop_share.py --display :99 --fps 15 --stats
```

验证结果：只发出时钟区域变化的小块（如 `41x56 @ (359,54)`、`69x38 @ (358,75)`），而不是整屏，
说明脏区检测按预期工作。

## 相关

- 脚本的实测带宽、触摸数据、完整验证记录：[scripts-measurements.md](scripts-measurements.md)
- 离线转换器 `pudcodec` 与一致性对拍：[pudcodec.md](pudcodec.md)
- FPS 基准确认方法：[fps-bench.md](fps-bench.md)
