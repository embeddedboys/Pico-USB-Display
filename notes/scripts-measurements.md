# 脚本的实测数据与验收

> 这些数字都是用 `tools/` 的脚本在同一块板子上量得的；**给设备计时必须把编码放在循环外**，
> 且**绝对 MB/s 是会话属性**（同一块板同一类端口实测过 0.94 与 1.10 MB/s 两档），只有比值能搬。

## TL;DR

- EP1 带宽从 11 KB 到 444 KB 平坦在 **1.01–1.05 MB/s**（带宽受限，不是开销受限）。
- **全速设备别挂在 hub 后面**：经 480M hub 0.833 MB/s，直插 xHCI 根口 **1.132 MB/s**（+36%，全部来自拓扑）。
- 局刷由 USB 带宽决定；全刷由面板写入决定（153600 像素固定约 36 ms ≈ 4.3 Mpx/s）。
- 推送触摸报告间隔中位 **8.0 ms（≈125 Hz）**；空闲时读会超时（设备不发声）。
- `desktop_codecs.py` 的"模型列"**不是实测**，引用前先读脚本表头写的来源。

## 实测数据（供对照）

在同一块板子、`PIO_USE_DMA=1` 的固件上：

**EP1 带宽**（`test_ep1_throughput.py`）—— 从 11 KB 到 444 KB 平坦在 **1.01–1.05 MB/s**，
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

## 触摸（`test_touch_ep4.py`）

```bash
python3 tests/test_touch_ep4.py                    # 推送模式，听 10 s，边摸边打印
python3 tests/test_touch_ep4.py --mode poll        # REQ_EP4_IN 轮询模型
python3 tests/test_touch_ep4.py --calibrate        # 另给触摸包围盒，判断轴序/反向
```

设备主动推送 8 字节报告（布局见 [touch-ep4.md](touch-ep4.md)），所以主机只是不断读 EP4；
`--mode poll` 用 `REQ_EP4_IN` 每次取一帧，用来对比两种模型。汇总会打印报告数、x/y 范围、报告
间隔中位数、sequence 跳变。**空闲时读会超时，这是正常的（设备不发声）**。
`pud_usb.py` 里有 `parse_touch_report()`、`Display.touch_request()`、`Display.read_touch()`，
新脚本请复用。

> **已知假阴性**：推送模式在"面板空闲"时会收不到报告，脚本据此断定"固件不支持触摸"，而
> caps 的 `touch` 位与 poll 模式的 `version = 1` 都说明支持。判据应该用 caps 的 `touch` 位
> （或先 poll 一次）来区分"没人碰"和"没实现"。

## 屏上触摸反馈（`touch_draw.py`）

数字只能告诉你坐标存在，画出来才能告诉你对不对。这个脚本把画布画到面板上，并在**设备上报的
坐标处**打标记，所以轴序交换、反向、旋转不跟随、贴合偏移一眼就能看出来：

```bash
python3 tools/touch_draw.py                 # trace：标记跟着手指走（默认）
python3 tools/touch_draw.py --mode grid     # 40 px 网格 + 坐标标签，按下处打点
python3 tools/touch_draw.py --mode targets  # 依次点 5 个十字靶，输出每个靶的误差
```

- `trace`/`grid` 摸就行；`grid` 会在标记旁写上设备报的 `x,y`，直接读数。
- `targets` 依次标出四角与中心，点完打印 `dx/dy`、平均误差（也就是 `x_offs/y_offs` 能抵消的
  量）和 x/y 跨度（跨度不对说明轴交换或没点全）。
- 电容屏与玻璃贴合本来就有边缘偏移，**几个像素的平均误差是正常的**。

**实测**（2026-09，`--mode grid --rate 20`，10 分钟）：29 次触摸 / 193 个报告 / 29 个 release，
四角落点 (0,0)、(475,0)、(475,319)、(0,314)，横拖只变 x、竖拖只变 y，sequence 全程只跳 2 次，
**板子没有挂**。同一批数据在 [touch-ep4.md](touch-ep4.md) 里也记了一份。

**默认限速**：`--rate 20`（每秒最多 20 次面板更新）。报告是 8 ms 一个（≈125 Hz），到得比
`--rate` 快的报告会被合并进下一次更新 —— 画布内容不丢，只是传输延后。`--gap-ms` 默认 **0**：
2026-09 无调试器复测（3000/6000 × 32×32 与 150/300 帧桌面负载，都是 gap 0）全部 `errors=0`、
`CFSR`/`HFSR` 为 0，"小传输连发会挂"不成立，见 [todo.md](todo.md) 第 8 条。

## 桌面负载下的编解码器比较（`desktop_codecs.py`）

```bash
python3 tools/desktop_codecs.py                                          # 合成桌面，只算载荷
python3 tools/desktop_codecs.py --image shot.png                         # 用真实桌面截图
python3 tools/desktop_codecs.py --image shot.png --device --codec lz4     # 再上板测时间
```

按"桌面会脏的矩形"逐个比较（默认合成一帧 480×320 桌面；给了 `--image` 就用真实截图）。
`--device` 会把每个 band **先编码好**再计时（复用 `fps_bench.measure()`），并把编码耗时单独
打成一列 —— 手写 Python 的 QOI/RLE 编码器整帧要几十毫秒，混进计时就会得出错误结论
（见 [codec-selection.md](codec-selection.md) 的"测量纠正"）。**内容越真实越好**：合成桌面
偏平坦，会高估 LZ4 的压缩率。真实截图怎么取（板上 GNOME/Wayland 的坑）见
[codec-selection.md](codec-selection.md)。

> **脚本的"模型列"不是实测**：排序表括号里的时间曾是按写死的 `LINK_BYTES_PER_S` 常数算出来的
> **模型**，只有带 `median`/`bandwidth`/`encode` 的 `device_table` 才是设备实测。两者混在一张
> 表里引用过一次，让"QOI 基准"差了 15%（85.22 ms 对真值 100.05 ms）。脚本现在带 `--device`
> 会先用一次真实传输**标定**，并在表头写明来源（`measured in this run` /
> `a model ... NOT a measurement`）—— 引用任何时间之前先读那一行。**绝对速率是会话属性**
> （同一块板同一类端口实测过 0.94 与 1.10 MB/s 两档），只有比值能搬。


## 完整验证记录

### 开发机上的一次验证（2026-09-26，x86_64 原生 Linux）

环境：Ubuntu 24.04、Python 3.12 + 仓库内 `.venv`（pyusb 1.3.1 / Pillow 12.3.0 / numpy 2.5.3 /
lz4 4.4.5）、`60-pico-usb-display.rules` 已装（设备节点 0666）、设备 `2e8a:0001`。
全部命令都在仓库根目录下跑。

| 命令 | 结果 |
| --- | --- |
| `python3 tools/pud_usb.py` | 自检通过；QOI/RLE 与 C 库参考向量一致，band limit 报 **21835** |
| `python3 tests/test_codec_crosscheck.py` | 28 项断言 **27 通过**，唯一 FAIL 是上面那条已知的 LZ4 版本差异 |
| `open_device()`（读 caps） | `proto 2 / frame_max 65536 / decoder 3 / band_pixels 21835`；面板 `480x320 rotation 1 16bpp 50000kHz touch poll 10ms 70x40mm touch True` —— 与固件/驱动文档**逐字段一致** |
| `img_viewer.py --xres 480 --yres 320 assets/bootlogo.jpg`（当时那张） | `sent 8 bands, 29814 B in 52.6 ms`（8 带 = `ceil(320/45)`） |
| 同上，换成现在的开机图 `assets/bootlogo.png` | `sent 8 bands, 13074 B in 26.8 ms` —— **载荷与时间仍只有原来的一半** |
| `test_ep1_throughput.py` | 8~320 行的每个尺寸都是 **~0.816 MB/s** 一条平线（**经 hub**；直插根口 1.13 MB/s，见下） |
| `fps_bench.py --full --frames 20` | photo **6.19 fps**、noise **1.83 fps**，都是 `USB 受限` |
| `video_player.py --xres 480 --yres 320 --fps 15 --no-loop test.mp4` | `75 frames in 5.1 s -> 14.6 fps, 0.37 MB/s`（目标 15 fps，基本实时） |
| `desktop_codecs.py --device --codec qoi --frames 20` | 全屏 93893 B → **117.63 ms**；八个区域带宽都是 **~0.80 MB/s**（经 hub；根口 86.00 ms） |
| `codec_compare.py --codec qoi --frames 20` | solid 265.4 fps / gradient 24.2 / photo 6.1 / noise 1.8 —— 与 `fps_bench` 一致 |
| `xorg_desktop_share.py --frames 30 --stats` | `30 frames in 5.7 s -> 5.2 fps, 0.06 MB/s`；日志里**只发脏矩形** |
| `touch_draw.py --seconds 5`（无人触摸） | 画布推上去 `2610 B / 13.5 ms`，0 报告、`rc=0` |
| `test_touch_ep4.py --mode poll` | **`version = 1`** —— 固件确实上报触摸 |
| `test_touch_ep4.py`（push，有人触摸） | x `159..342` / y `162..225`、报告间隔中位 **8.0 ms**（≈125 Hz）、exit 0 |

吞吐 0.816 MB/s 比直连低约 28%，是**拓扑差异**（Pico 经 480M hub 的 TT），不是设备性能。

### 全速设备别挂在 hub 后面（2026-09-30 A/B 实测）

同一台 x86 开发机、同一块板子、同一份固件（`DECODER_TYPE=3`，225 MHz），只换插口：

| 插法（sysfs 路径） | `test_ep1_throughput.py` 11 KB / 88 KB / 444 KB | 桌面整屏 93893 B（`desktop_codecs.py --device`） |
| --- | --- | --- |
| 经 480M hub（`3-2.2`，走 TT） | 0.817 / 0.833 / 0.833 MB/s | 117.63 ms（2026-09-26 那轮） |
| **直插 xHCI 根口**（`3-6`） | **1.110 / 1.130 / 1.132 MB/s** | **86.00 ms**（1.09 MB/s） |

- **+36% 吞吐（0.833 → 1.132 MB/s），全部来自拓扑**；根口的数字 = 直连 = 全速理论上限
  1.216 MB/s 的 93%。两种插法下设备侧都干净（`submitted == drawn`、`dropped`/`bad`/
  `oversize`/`stale` = 0、`CFSR`/`HFSR` = 0），所以损失在 hub 的 TT 调度里，不在设备。
- **量吞吐前先看拓扑**：`/sys/bus/usb/devices/<路径>/speed` 是 `12`（设备本身全速），关键是
  路径里有没有 `.`（`3-2.2` = 经 hub、`3-6` = 根口），或 `lsusb -t` 里它上面是不是一个
  `Class=Hub`。拿经 hub 的数字和别人直连的比，会把 28% 的拓扑差当成设备问题。
- 根口下各区域（20 帧中位，编码在计时外）：面板条 2122 B 2.06 ms、文本行 2010 B 1.96 ms、
  终端窗体 18124 B 17.00 ms、壁纸条 19252 B 17.64 ms、整屏 86.00 ms —— 带宽 1.03~1.11 MB/s，
  仍然**贴着链路**，要再快只能少发字节。
- `img_viewer.py` 打印的 MB/s（根口下 0.64~0.81）**不是链路速度**：它的计时里含 Python 侧的
  图片加载与 QOI 编码。

## 相关

- 脚本用法与依赖：[scripts.md](scripts.md)
- 离线转换器 `pudcodec`：[pudcodec.md](pudcodec.md)
- 选型与测量纠正：[codec-selection.md](codec-selection.md)
