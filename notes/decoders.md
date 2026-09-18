# 解码器与帧流水线

## 解码器抽象

编译期通过 `DECODER_TYPE` 选择（`CMakeLists.txt` 里的 `set(DECODER_TYPE ...)`，默认 **3 = QOI**）：

| 值 | 名称 | 输入格式 | 状态 |
| --- | --- | --- | --- |
| 0 | tjpgd | JPEG | 可用（ChaN TJpgDec，见下）；局刷正确但慢 |
| 1 | JPEGDEC | JPEG | 可用且更快，但**局部刷新会把显示路径写死**（见下） |
| 2 | LZ4 | LZ4 | 可用；**每个传输一个 band**（block 不能分块解码，见下） |
| 3 | **QOI** | RGB565 QOI | **当前使用**（全屏比两种 JPEG 快 12~20 倍） |
| 4 | RLE | RGB565 RLE | 可用；高熵内容比 QOI 小，结构化内容比 QOI 大（见下） |

### LZ4（2026-09 重新设计）

**为什么有 LZ4**：Linux 内核自带 LZ4（`lib/lz4/`，`LZ4_compress_default()` 与
`LZ4_decompress_safe()` 都 `EXPORT_SYMBOL`），驱动用它就不必把 QOI/RLE 的编解码源文件
vendor 进内核。所以设备侧这条解码路径必须真的能用。

**为什么不能像 QOI/RLE 那样解**：LZ4 的 block 格式里每个 match 都指回**同一 block** 之
前产生的输出，所以一个 block 必须一次性落进**一块连续缓冲**，这块缓冲同时就是字典。
整帧 480×320 = 307200 B，旧实现 `malloc(LZ4_compressBound(frame))` ≈ 308 KB 在设备上
永远失败（可用 SRAM 只有 ~290 KB），每帧还有 3 行 `printf`（115200 波特下约 3 ms）。

**现在的设计**：设备只持有**一个 band**，不是一帧。主机按**和 QOI/RLE 完全相同**的规则
分带（`band_pixels`，来自 `PUD_CMD_GET_CAPS`），每个传输里的 LZ4 block 就是该矩形自包含
的码流；`lz4_drawimg()` 解到静态 `lz4_band[]` 里再整带刷屏。**协议没有新增字段**：band
就是这个传输的矩形，和其它解码器一样。

```c
#define LZ4_BAND_PIXELS (((PUD_MAX_TRANSFER - 16) / 3) + 1)   /* 21840 px = 43680 B */
static uint16_t lz4_band[LZ4_BAND_PIXELS];
```

缓冲按主机分带用的同一个规则定尺寸（多留 1 像素，覆盖两侧取整差异），所以主机按
`band_pixels` 发的任何 band 都放得下。放不下或解码长度与窗口不符时**丢弃并计数**，
不截断：

| 计数（gdb 可读） | 含义 |
| --- | --- |
| `g_decoder_stat_lz4_oversize` | band 比 `lz4_band[]` 大（主机分带太粗） |
| `g_decoder_stat_lz4_bad` | 解码长度 ≠ 窗口像素数（截断/损坏/窗口与码流不符） |

**开机 logo**：LZ4 构型不能用一个整帧 block，所以它的 logo 是**band 容器**——
`[count][offsets][blocks]`，每个 band 一个 block，band 高度由 `height / count` 推出
（`decoder_draw_bootlogo()`）。生成方式见 [scripts.md](scripts.md)，重新生成的这一支
**同时修掉了**"LZ4 分支与其它三支不是同一张图"的老问题。

**实测（480×320，分带，`scripts/codec_compare.py`，20 帧）**：

| 内容 | LZ4 字节/帧 | LZ4 端到端 | QOI | RLE |
| --- | --- | --- | --- | --- |
| solid | 1297 | **7.00 ms** | 7.99 ms | 5.69 ms |
| gradient | 29971 | **30.73 ms** | 35.96 ms | 47.71 ms |
| photo | 171689 | 157.33 ms | **127.49 ms** | 206.98 ms |
| noise | 308417 | 276.40 ms | 422.85 ms | **275.74 ms** |

LZ4 的压缩率在结构化内容上明显好于 RLE、和 QOI 各有胜负，高熵内容与 RLE 相当。
代价（LZ4 构型）：静态 RAM 247276 B（QOI 构型 218944 B，多出的就是 43680 B 的
`lz4_band`，同时少了其它解码器的乒乓缓冲），flash 因为下面的 `--gc-sections` 修正反而更小。

**还没做**：整带解完才刷屏，解码与面板传输没有重叠（QOI/RLE 靠乒乓重叠了）。要重叠得再
加一块 band 缓冲（+43680 B）并去掉 drawimg 末尾的 wait，收益只在纯色这种"载荷极小、
解码占比大"的内容上，未测。

> **LZ4 码流不是跨版本逐字节一致的**：vendored 的是 liblz4 1.10.0，板子上 python-lz4
> 4.4.5 带的是 1.9.x，同一条 band 有 3/8 压缩结果不同（都是合法 block）。设备只解压
> （`LZ4_decompress_safe` 与版本无关），两条来源的码流**都在板上验证过像素精确**。

### 桌面负载下的选型（2026-09 实测，真实桌面）

项目面向桌面（配合 DRM 驱动做显示镜像），主负载是**局部刷新**：整屏照片/噪声测试不代表
它。`scripts/desktop_codecs.py` 按“桌面会脏的矩形”逐个比较，内容用**板子自己的真实桌面**
——HDMI 会话截屏（3840×2160），两种用法各测一遍：

- `real_downscaled`：整屏缩到 480×320（整屏镜像）
- `real_crop`：4K 截屏里 1:1 裁 480×320（原生分辨率 UI，文字可读）

板上实测（20 帧中位值，间隔 3 ms，`--device`；**编码在计时循环外**、单独列出）：

| 区域 | 像素 | QOI | RLE | LZ4 | QOI 载荷 | LZ4 载荷 |
| --- | --- | --- | --- | --- | --- | --- |
| 面板条 480×24 | 11520 | **3.20 ms** | 5.15 | 4.50 | 3318 B | 4823 B |
| 文本行 272×14 | 3808 | **2.94** | 4.94 | 3.94 | 2878 | 4128 |
| 列表行 240×16 | 3840 | **1.84** | 3.06 | 2.49 | 1707 | 2523 |
| 标题栏 260×22 | 5720 | **3.52** | 6.20 | 5.09 | 3649 | 5233 |
| 窗口空白体 260×60 | 15600 | **6.89** | 11.88 | 10.04 | 7435 | 10968 |
| 整窗 288×140 | 40320 | **25.92** | 41.94 | 32.61 | 26391 | 35449 |
| 壁纸条 480×60 | 28800 | **13.42** | 22.94 | 19.77 | 14398 | 20586 |
| 整屏 | 153600 | **75.70** | 120.97 | 100.92 | 76503 | 107650 |

**QOI 每个矩形都快 21~32%，因为它载荷最小（少 15~35%），而三者都跑在链路极限上**：
实测带宽 QOI 1.01、LZ4 1.07、RLE 1.09 MB/s —— 链路约 1.1 MB/s，时间基本就是
`载荷 ÷ 带宽`。桌面内容（抗锯齿文本、阴影、半透明、照片壁纸）**不是** LZ4 擅长的
长重复模式，而是 QOI 的 index/diff/luma 更能描述的东西。

设备侧（`DECODER_STATS` 的 `draw_us`，即解码+刷屏，不含主机）每帧：

| 整屏 153600 px | QOI | RLE | LZ4 |
| --- | --- | --- | --- |
| 设备忙 | 5.5 ms | 2.4 ms | 2.2 ms |

远小于各自的链路时间（76 / 121 / 101 ms），所以**当前是链路受限，QOI 的解码慢不构成问题**；
但这是它唯一的隐患：链路一旦提速（USB 高速、更快面板），QOI 会先变成设备侧瓶颈。

> **测量纠正（重要）**：第一版 `desktop_codecs.py --device` 用 `send_rgb565()` 计时，而它
> **在计时循环里做编码**。`pud_usb.py` 的 QOI/RLE 编码器是手写 Python（实测整帧
> **43.8 / 54.8 ms**），LZ4 走 liblz4（**2.6 ms**），于是量到的是"Python 解释器 vs C 库"，
> 当时得出"LZ4 快 6~20%"的结论是**错的**。现在改成先把每个 band 编码好、再用
> `fps_bench.measure()`（只发 `send_raw`）计时，并把编码耗时单独打印成一列。
> 内核驱动里两边都是 C（驱动自己编码），不存在这个偏差。

**怎么取真实桌面内容**（板上 GNOME/Wayland 的三个坑）：

- GNOME 的截图 D-Bus（`org.gnome.Shell.Screenshot`）**拒绝跨会话调用**：ssh 的会话与
  图形会话不是一个 session，报 `Screenshot is not allowed`，`systemd-run --user --scope`
  也一样；
- `/dev/fb0`（`rockchipdrmfb`）读到的是**全黑** —— 合成器扫的是自己的 GBM buffer，
  fbdev 模拟那块不是活画面；
- 所以用**键盘 PrintScreen**（GNOME 存到 `~/Pictures/Screenshots/`），拷出来传给
  `desktop_codecs.py --image` 即可。

**选型结论（设备侧单独看）**：桌面负载上 **QOI 最快**；纯照片整屏也是 QOI 的场合。
LZ4 的价值不在性能，而在**内核侧**——它是内核自带的，驱动不必 vendor 编码器源文件
（见 [todo.md](todo.md) 第 7 条）。要换 LZ4 就得接受桌面内容上约 **25% 的额外延迟**
（100.92 vs 75.70 ms 整屏），这笔账按项目优先级算。

### 批次乒乓值多少（A/B 实测）

QOI/RLE 的回调解码器可以只用一块累积缓冲（每批都要等它刷完）或用两块乒乓（下一批的
解码和上一批的刷屏重叠）。`PUD_DECODER_PINGPONG`（默认 1）把它做成构建开关，可以随时
A/B；关掉还能省 **7680 B/活跃解码器**（QOI 构型实测 `.bss` 152132 → 144456）。

**设备侧确实有效**（`DECODER_STATS` 的 `draw_us`，解码+刷屏，不含主机；每帧之间有 2 ms
间隔，两者同条件）：

| 载荷 | 内容 | 乒乓开 | 乒乓关 | 差 |
| --- | --- | --- | --- | --- |
| QOI 2634 B | 纯色整屏 | 765 µs/带 | 1110 µs/带 | **-31%** |
| RLE 3638 B | 纯色整屏 | 491 µs/带 | 811 µs/带 | **-39%** |
| QOI 76503 B | 真实桌面整屏 | 5498 µs/帧 | 5897 µs/帧 | -7% |
| RLE 131723 B | 真实桌面整屏 | 2424 µs/帧 | 2777 µs/帧 | -13% |

**端到端在那个负载下量不到差别**（0.1~0.7%，在噪声里）：纯色整屏 20.95 → 21.09 ms、
真实桌面整屏 113.37 → 113.76 ms —— 因为瓶颈在主机/链路，设备侧那点时间被藏住了。

**什么时候能看到**：设备侧成为瓶颈时。早先异步刷新的那次实测就是这个场景（紧循环、
载荷极小、单缓冲退化成同步刷屏）：**帧率 123.8 → 177.5 fps（8.00 → 5.63 ms）**，详见
下面"2. 异步刷新"。

**结论**：留着（默认开）。它的收益是**设备侧余量**——纯色/低熵内容、以及 RP2040 那种
更慢的核——而不是现在这条链路下的帧率；代价是一个 7680 B 缓冲，紧的时候用
`-DPUD_DECODER_PINGPONG=0` 换回来。

> 紧循环那一栏是**无间隔**跑的（`--gap-ms 0`）。旧笔记说这样会把板子的 USB 打挂，
> **已否定**：无调试器复测（3000/6000 × 32×32 无间隔、150/300 帧桌面负载）全是
> `errors=0`，跑完 `CFSR`/`HFSR` 仍为 0，见 [todo.md](todo.md) 第 8 条。

### RGB565 RLE 与 QOI（2026-09 实测）

RLE 来自独立仓库 `rgb565-rle`（MIT），vendored 在 `src/decoders/rle/`，与该仓 `master`
逐字节一致（含下面的 run 批量填充优化）。
接口形状与 QOI 库一样（回调 + 双缓冲 ping-pong），**零静态状态**，所以不额外吃 RAM。

| | RLE | QOI |
| --- | --- | --- |
| 格式 | 4 B 像素数 + 重复 [控制字节 + 数据] | `q565` 魔数 + 逐 op |
| 理论最坏 | 3 B/px | 3 B/px（所以分带规则不用改） |
| 480×320 纯色 | 3638 B | **2634 B** |
| 480×320 渐变 | 49992 B | **32873 B** |
| 480×320 照片 | 226758 B | **131183 B** |
| 480×320 **噪声** | **308433 B** | 444550 B |
| 端到端 480×320（分带） | 6.73 / 48.36 / 207.0 / 275.9 ms | 7.00 / 35.70 / 127.4 / 422.7 ms |
| 端到端 160×120（单次传输） | 0.46 / 2.32 / 9.10 / 12.00 ms | 0.53 / 1.69 / 5.58 / 17.95 ms |
| 静态 RAM | 与 QOI **完全相同**（整机构型 222676 B） | — |
| `decoder_task` 栈峰值 | 464 B | 496 B |
| flash（text+data） | 177888 B | 158992 B |

（端到端四列依次是 solid / gradient / photo / noise；由 `scripts/codec_compare.py` 测，
内容是同一份，只换编码器。）

**结论：链路是瓶颈（EP1 约 1.1 MB/s），所以端到端快慢基本由压缩率决定。** 结构化内容
QOI 小 1.4~1.7 倍（更快）；**高熵内容 RLE 反而小 1.45 倍** —— 它的 literal run 能把 128 个
像素摊到一个控制字节（实测噪声 2.01 B/px），而 QOI 对高熵像素只能发 3 B/px 的 RGB565 op。
解码本身两种都很快：160×120 单次传输（一帧一次传输）下差距 0.1~6 ms，且仍主要被传输时间
占掉，所以选型看的是压缩率而不是解码速度。

RLE 是**无损**的，而且**板上验证过**：开机 logo 整帧 480×320、153600 个像素与参考逐像素
完全相等（同一路径下 QOI 也是精确相等；tjpgd/JPEGDEC 是 ±1 LSB）。

### RLE 解码器的 run 批量填充（2026-09 实测）

上游 RLE 的 run 体是逐像素 16 位 store（重复 run 一个 `strh`、literal run 一次
`read_pixel_le` + store），而一个 run 最多 128 像素 —— 循环开销要按控制字节摊薄之前得先付
128 次。改成每次处理 4 个像素：重复 run 存一个"同一个像素写两遍"的 32 位字，literal run
直接字节拷贝（小端目标上压缩数据本身就是像素值，不用换字节序），尾部按 2 个或 1 个像素
收尾。**只动 run 体**：每个 helper 拿到的长度都是调用者已经切好的，所以缓冲填充点、flush
时机、回调参数一个都没变 —— 这正是流式 API 承诺的分批行为。

板上 A/B（150 MHz RP2350，480×320 按 8 行分批，交替烧写、同一主机同一脚本、`--frames 100`；
"解码+刷屏"取 `DECODER_STATS` 的 `draw_us` 每帧均值）：

| 内容 | 解码+刷屏（改前 → 改后） | 端到端（改前 → 改后） |
| --- | --- | --- |
| solid | 4460 → **3985 µs**（-10.7%） | 6.00 → 6.00 ms |
| gradient | 15525 → 15457 µs（-0.4%） | 48.03 → 48.26 ms |
| photo | 40708 → 39691 µs（-2.5%） | 207.68 → 207.21 ms |
| noise | 46230 → 45156 µs（-2.3%） | 276.32 → 276.03 ms |

**设备侧确实更快，端到端却看不出来**：四档里三档是 USB 受限（解码藏在传输后面），只有 run
重的纯色内容解码占比大，省下的 0.5 ms 也基本被分批流水线吸收。保留它的理由是设备侧余量
（局刷/小窗口，以及 133 MHz 的 M0+ 更吃解码），不是现在的帧率。

正确性：与改前的实现对拍 **28560 例**（7 种内容 × 宽 1~480 × 高 1~320 × 10 种批容量 ×
单缓冲/乒乓），整帧解码结果**和完整回调事件序列**（坐标、像素数、缓冲身份、像素数据）
逐位相同；板上开机 logo 整帧 153600 像素仍是 0 差异。

> 这份改动已提交到 `rgb565-rle` 仓（**未推送**）。重新 vendor 时要注意：从远端拉会丢掉它，
> 直到该提交推上去为止。


`include/decoder.h` 用一个宏做分发：

```c
#define decoder_drawimg(xs, ys, xe, ye, b, l) qoi_drawimg(xs, ys, xe, ye, b, l)
```

调试日志里的名字来自 `decoder_names[] = { "tjpgd", "JPEGDEC", "LZ4", "QOI", "RLE" }`，
开机打印 `Decoder type: QOI`。

> **坑**：这个数组曾漏掉 `"QOI"` 这一项，而 `DECODER_TYPE=3` 会越界读。
> 加解码器时别忘了同步这个数组；**编号不要重排**（`decoder_type` 会上报给主机）。

### 两种 JPEG 实现（2026-09 实测）

| | tjpgd（0） | JPEGDEC（1） |
| --- | --- | --- |
| 来源 | ChaN TJpgDec R0.03 + Bodmer 的 `swap`，vendored 在 `src/decoders/tjpgd/` | `src/decoders/jpegdec/` |
| 480×320 4:4:4 全屏（`assets/bootlogo.jpg`） | 114.6 ms | **76.4 ms** |
| 480×320 4:2:0 全屏（Pillow 重存） | 175.3 ms | **66.1 ms** |
| 局部刷新（`x != 0`，64×64 @ x=208） | ✓ 正确 | ✗ **坐标错并卡死显示** |
| 额外 RAM（净） | +2 KB | 0 |
| 解码栈峰值 | 600 B | 632 B |

两者的语义都是**主机把子图裁好、JPEG 自带尺寸、固件按 `(xs,ys)` 贴图**，`xe/ye` 不参与。

**为什么两种都留着**：JPEGDEC 在 `x != 0` 时 `iWidthUsed` 会算出负值，`draw_mcus` 于是把
`xe = x + iWidth - 1` 填成**子图内坐标**（实测 `xs=208 → xe=63`），`len` 变成巨大的无符号数；
一次这样的 flush 就足以让 `decoder_task` 卡在 `tft_video_flush` 里，帧槽永不释放、EP1 永不
重新武装（实测 `submitted/drawn = 2/0`、`s_ep1_pending_size` 一直挂着），主机只能超时。
**规范：JPEG 两条路都只用于整屏；局刷一律走 QOI。**

tjpgd 侧踩过的坑：

- 上游按 **8×8 块**回调，逐块 flush 会把地址窗口设 **2400 次/帧**；改成攒满 8 行再 flush
  （`TJPGD_GROUP_ROWS`，缓冲 480×8×2 = 7.5 KB）后每帧 40 次 —— 但实测只快 3~7%
  （118.5 → 111~115 ms），说明瓶颈在解码核心，不在刷屏粒度。
- **MCU 高度随色度采样变化**（4:4:4 → 8 行，4:2:0 → 16 行），一次回调可能跨 8 行组的边界，
  所以拷贝必须**按行遍历、跨组即 flush**；按“一块一次”写会按 16 行写进 8 行缓冲 ——
  480 宽的 4:2:0 图会写穿 7.5 KB。`assets/bootlogo.jpg` 恰好是 4:4:4，只用它测发现不了。
- 净 RAM 只有 +2 KB：9.4 KB workspace（`JD_FASTDECODE 2`）+ 7.5 KB 行缓冲，替换掉了同一
  构建里 QOI 的 15 KB 乒乓缓冲（`qoi_buf_a/b`，靠 `--gc-sections` 丢弃）。

**各解码器的 `drawimg(xs, ys, xe, ye, data, size)` 签名一致**，坐标为整屏绝对坐标，
`xe`/`ye` 为闭区间。

## 帧流水线

```
USB 中断 (EP1 完成)
   │  usbd_vendor_ep1_bulk_out()
   ▼
decoder_submit_frame(xs, ys, xe, ye, ep1_read_buffer, nbytes)
   │  找一个空闲帧槽，memcpy 进去，give 信号量
   │  （满了就丢帧并计数 —— 但流控已使这不再发生）
   ▼
decoder_task()  ← 独立任务，栈 1024 words（4 KB）
   │  （开机时先在这里画一次 logo，然后才进循环）
   │  take 信号量 → 找到 busy 槽
   │  decoder_drawimg(...)   ← 真正解码 + 刷 TFT
   │  槽置空闲 → usbd_vendor_ep1_tick()  ← 补发被延迟的 EP1 武装
   ▼
TFT
```

### 帧槽

```c
#define DECODER_FRAME_SLOTS 2
#define DECODER_FRAME_MAX   65536        /* 每槽 64 KB */

struct decoder_frame {
    u16 xs, ys, xe, ye;
    u32 size;
    u8  busy;
    u8  data[DECODER_FRAME_MAX];
};
static struct decoder_frame s_frames[DECODER_FRAME_SLOTS];
```

两个槽 = 128 KB 静态 RAM。**一帧的压缩结果必须 ≤ 64 KB**，否则 `decoder_submit_frame()`
会截断（`size > DECODER_FRAME_MAX` 时钳位）—— 截断的流会解码失败，表现为一块区域不更新。
主机的分带机制保证单帧不超过这个上限（见驱动侧 `display-and-refresh.md`）。

**为什么只有 2 个槽**：RP2350 只有 512 KB SRAM，而 `EP1_RD_BUF_SIZE` 已经占了 128 KB。
2 × 128 KB 的槽会直接溢出预算（详见 [pitfalls.md](pitfalls.md)）。

### 为什么解码必须放在任务里

`usbd_vendor_ep1_bulk_out()` 运行在 **USB 中断上下文**。早期版本直接在这里调用解码，
导致 HardFault：

```
CFSR 0x8200 (STKERR + INVSTATE)
faulting PC: usbd_ep_start_read+126, r2 = <TFT 数据>
```

原因是 JPEGDEC 解码占用大量栈，USB 中断栈放不下（STKERR = 压栈失败），
且耗时操作阻塞了 USB 中断。**结论：中断里只做搬运，解码一律交给 `decoder_task`。**

### 流控（背压）

槽满时固件**不武装 EP1**，让主机的 bulk 传输阻塞等待，而不是丢帧。
实现与成立前提见 [usb-protocol.md](usb-protocol.md) 的"EP1 图像帧的处理（含流控）"。

**这是修掉"局部刷新有残影"的关键改动。** 效果实测（gdb 读计数器）：

| 场景 | submitted | dropped | drawn |
| --- | --- | --- | --- |
| 桌面动画（约 5 s） | 2051 → 2772 | **0** | 2050 → 2770 |
| 桌面动画（约 1 s） | 3876 → 4593 | **0** | 3876 → 4593 |
| fbdev 压测（150×16 KB） | 7445 → 8860 | **0** | 7444 → 8860 |

`drawn` 恰好落后 `submitted` 1 帧（那 1 帧在途），说明流水线严格单帧深度 —— 这是流控生效的特征。

### 诊断计数器

```c
volatile u32 g_decoder_stat_submitted;   /* 收到的帧数 */
volatile u32 g_decoder_stat_dropped;     /* 因无空闲槽被丢弃的帧数 */
volatile u32 g_decoder_stat_drawn;       /* 已完成绘制的帧数 */
volatile u32 g_decoder_stat_oversize;    /* 载荷装不进帧槽的帧数 */
volatile u32 g_decoder_stat_lz4_oversize;/* LZ4: band 装不进 lz4_band[] */
volatile u32 g_decoder_stat_lz4_bad;     /* LZ4: 解码长度与窗口不符 */
```

声明为 `volatile` 以便调试器读取而不被优化掉。用法见 [debugging.md](debugging.md)。

**只要 `dropped` 不为 0，就说明流控失效**（例如有人改回了"直接武装 EP1"），
症状就是局部刷新残影。

## QOI 解码实现

QOI 的**格式规范完整写在 `src/decoders/qoi/rgb565_qoi.h` 的文件头注释里**
（magic、五种 chunk、hash 函数、最坏情况大小推导）—— 这里不重复。

固件用的是**流式回调**接口，避免在 MCU 上分配整帧缓冲：

```c
#define QOI_BUF_ROWS 8
static uint16_t qoi_buf_a[480 * QOI_BUF_ROWS];   /* 7680 px = 15360 B */
static uint16_t qoi_buf_b[480 * QOI_BUF_ROWS];   /* 另一个做 ping-pong */

void qoi_drawimg(u16 xs, u16 ys, u16 xe, u16 ye, u8 *qoi_data, u32 qoi_size)
{
    struct qoi_draw_ctx ctx;
    uint16_t width = xe - xs + 1;
    size_t buf_cap;

    if (qoi_data == NULL || qoi_size == 0 || width == 0 || width > 480)
        return;

    ctx.ox = xs;
    ctx.oy = ys;

    /* 批次按整行对齐：宽度的整数倍，上限为静态缓冲 */
    buf_cap = (size_t)width * QOI_BUF_ROWS;
    if (buf_cap > 480 * QOI_BUF_ROWS)
        buf_cap = 480 * QOI_BUF_ROWS;

    rgb565_qoi_decompress_callback(qoi_data, qoi_size, width,
                                   qoi_buf_a, qoi_buf_b,
                                   buf_cap, qoi_flush, &ctx);
}
```

回调把解出的批次刷到 TFT：

```c
static void qoi_flush(const uint16_t *pixels, size_t count,
                      uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye,
                      void *user_data)
{
    struct qoi_draw_ctx *ctx = user_data;
    tft_video_flush(ctx->ox + xs, ctx->oy + ys,
                    ctx->ox + xe, ctx->oy + ye,
                    (void *)pixels, count * 2);
}
```

**坐标细节**：QOI 流内部坐标是相对于该子图的 `(0,0)`，所以回调必须加上
`ctx.ox`/`ctx.oy` 换算成整屏绝对坐标。

**批次粒度按整行对齐**（`width * QOI_BUF_ROWS`），这样每次回调刷的是一个矩形块，
而不是跨行的散点 —— 对 TFT 的窗口设置更友好。

**两级 ping-pong**：库内部在第一/第二缓冲间切换（一个被回调消费时另一个继续填），
让刷屏与解码尽量重叠。

### 为什么从 JPEG 换成 QOI

| | JPEG (JPEGDEC) | QOI |
| --- | --- | --- |
| 有损 | 是 | **否** |
| 子图解码正确性 | MCU/crop 逻辑在 `x != 0` 时错位裁切，且会卡死显示（见上） | **按像素处理，任意子矩形都正确** |
| 编码速度（主机） | 慢 | **极快** |
| 压缩率 | 好 | 差（照片类约 1 B/px，最坏 3 B/px） |

压缩率差通过主机侧**分带**来补偿（每带 ≤ 21839 像素）。详见
`PUD-kernel-drivers/notes/display-and-refresh.md`。

### JPEGDEC 路径的遗留 bug（若要用回 JPEG）

`draw_mcus` 里必须用 `pDraw->iWidthUsed` 而不是 `pDraw->iWidth`：

```c
/* 错误 */  pDraw->iWidth
/* 正确 */  pDraw->iWidthUsed
```

`iWidth` 是整图宽度，`iWidthUsed` 才是本次实际解码的宽度。用错会导致每行像素偏移错位。

**实测比“错位”更严重**：给 `x != 0` 的矩形发 JPEG 时 `iWidthUsed` 会变成负值，`xe` 被算成
子图内坐标、`len` 变成巨大的无符号数，一次这样的 flush 就把 `decoder_task` 卡死在
`tft_video_flush` 里（详见上文“两种 JPEG 实现”的实测）。所以 **JPEG 只用于整屏（`x = 0`）**。

## 性能：解码器批量填充 + 异步刷新

两处优化，都有实测数据。全屏纯色的端到端耗时从 **36.27 ms 降到 5.63 ms（6.4×）**。

### 1. RUN chunk 批量填充（库内 `rgb565_qoi.c`）

`rgb565_qoi_decompress_callback()` 原本逐像素迭代，`EMIT_PIXEL` 对每个像素都重做
行环绕与容量判断。RUN chunk 因此把同样的簿记重复了 run 次。

改法：**只改 RUN 分支**，把重复像素用一段紧凑内层循环填完（`EMIT_RUN`），
并被 clamp 到图像末尾；冲刷点与原来完全一致。

| 内容（480×320 全帧） | 解码耗时 | 每像素 |
| --- | --- | --- |
| RUN 密集（纯色/渐变） | 32.65 ms → **5.0 ms** | 213 → **32.7 ns/px** |
| RGB565 密集（噪声） | — | 499 → **473 ns/px** |

**踩坑记录（很重要）**：第一版重构把 `EMIT_PIXEL` 从 5 个分支里合并成一份，
结果噪声类内容在 Cortex-M33 上**慢了 20%**（499 → 600 ns/px）。原因不是算法，而是
**寄存器分配**：原版每个分支各展开一份，`buf` 指针能驻留在寄存器；合并后它必须跨
分派存活，被编译器挤到栈上（EMIT 快路径从约 8 条指令涨到约 14 条）。
所以这里刻意**保留了每个分支各展开一份 EMIT_PIXEL 的写法** —— 用最小改动换性能。

> 教训：x86 上的 A/B（`gcc -O2/-O3`）**复现不了**这个回退（主机上反而更快），
> 因为编译器差异太大。这类寄存器分配问题只能看 `arm-none-eabi-gcc` 的汇编或上机实测。

### 2. 异步刷新（`pico-display-lib` 新增接口，不改原有接口）

原来 `i80_write_buf_rs()` 在 `dma_channel_wait_for_finish_blocking()` 上阻塞，
CPU 干等总线。新增的异步接口把这次等待挪到**下一次调用**，中间的空档留给解码：

| 接口 | 位置 | 说明 |
| --- | --- | --- |
| `i80_write_buf_rs_async(buf, len, rs)` | `drivers/bus/pio_i80.c` | 启动 DMA 后立即返回 |
| `i80_write_sync()` | 同上 | 等待在途传输并完成尾部 CS 释放 |
| `tft_async_video_flush(...)` | `drivers/display/tft.c` | 异步版 `tft_video_flush` |
| `tft_async_video_wait()` | 同上 | 帧末等待 |

**引脚时序与同步路径完全一致**：异步版在下次调用的开头等 DMA、再释放 CS，
而同步版是在调用内部等 —— 只是等待的位置变了。因此显示波形不变。

**缓冲区复用安全**：同一时刻只允许一个传输在途（新传输开始前先完成上一个）。
QOI 解码器的 `qoi_buf_a`/`qoi_buf_b` 乒乓因此天然安全 —— 一个缓冲再次被写入前，
中间那次 flush 已经等过它了。`qoi_drawimg()` 在解码返回后调用
`tft_async_video_wait()`，保证这一帧真正画完。

| 指标（全屏纯色） | 同步 | 异步 |
| --- | --- | --- |
| 帧率 | 123.8 fps（8.00 ms） | **177.5 fps（5.63 ms）** |
| partial 64×64 | 3.69 ms | **3.19 ms** |
| partial 128×64 | 6.99 ms | **6.31 ms** |

稳定性：45 秒混合重载 646 次传输（median 57.77 / p99 58.01 / max 58.46 ms，无超时）、
9000 帧高频局刷，全程 `dropped=0` 且 `drawn == submitted`。

**上限在哪**：显示路径实测 21.9 ns/px，而 PIO 理论值是 20 ns/px（`clk_div=1.5`、
`out+nop` 两周期）—— 已经跑在总线极限上，所以异步能拿回的只有这部分重叠收益，
再往上要改的是总线本身（提高 `TFT_BUS_CLK_KHZ` 或换总线）。

### 诊断开关

`src/decoders/decoder.c` 的计时计数器默认关闭，需要时打开：

```bash
cd build-pico2 && cmake .. -DPICO_BOARD=pico2 -DDECODER_STATS=1 && cmake --build . -j8
```

然后按帧读增量（只能增，测一段已知负载的差值）：

```gdb
printf "%u %u %u %u %u\n", g_qoi_stat_draw_us, g_qoi_stat_flush_us, \
       g_qoi_stat_pixels, g_qoi_stat_calls, g_qoi_stat_frames
```

注意 `g_qoi_stat_flush_us` 在异步模式下**不再等于总线时间**（调用立即返回），
要看总时间用 `draw_us`。

### 命令与数据的顺序（异步接口的安全前提）

一个必须明确的点：**地址窗口这类命令不能走异步路径**，否则命令与在途数据可能乱序。
本实现的做法是：

- 所有命令（`set_addr_win`、`write_reg`、`tft_write_cmd/data`）都走**同步**宏
  `write_buf_dc` → `i80_write_buf_rs()`；
- 异步宏 `write_buf_dc_async` 在整库里**只有一处调用**：`tft_async_video_flush()`
  里的像素数据；
- 同步入口的第一步就是 `i80_finish_pending()` —— 先等在途异步传输的 DMA 完成并释放 CS，
  再 `i80_wait_idle()` 等 PIO 排空，然后才拉 CS/RS 发命令。所以命令**不可能越过**在途数据；
- 命令→数据的边界同理：`set_addr_win` 最后一个字节（`0x2C`）同步发完后，
  异步入口同样先 `finish_pending()` + `wait_idle()` 才切 RS=1 启动数据 DMA。

这与原同步路径的步骤**完全一致**，只是等待发生的位置不同，因此引脚时序不变。

**测试缺口与补法（重要）**：`fps_bench.py` 的局刷用例**窗口位置是固定的**，
这种情况下即使窗口命令真的乱序、画面也看不出问题（每次重发的 CASET/RASET 值相同）。
所以另有一个**窗口逐帧移动**的测试：每帧先把上一位置擦成背景色、再画到新位置，
正确时应始终只看到一个块；任何乱序都会表现为拖影、两个块或位置错一帧。

实测：807 次传输（8 段背景 + 799 个块）精确计数、`dropped=0`、`drawn == submitted`，
且人眼确认「只有一个亮块在跳、无拖影」。
