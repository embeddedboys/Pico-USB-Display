# 解码器抽象与 JPEG 实现

> `DECODER_TYPE` 在编译期选定实现，`include/decoder.h` 用一个宏把
> `decoder_drawimg(xs, ys, xe, ye, data, size)` 分发到对应库；所有实现的签名与坐标系一致。

## TL;DR

- 各实现的签名相同：坐标为**整屏绝对坐标**，`xe`/`ye` 为**闭区间**（宽度 = `xe - xs + 1`）。
- `decoder_names[]` 必须覆盖每一个 `DECODER_TYPE`（现为 7 项，带 `_Static_assert`）；曾漏 `"QOI"` 导致 `DECODER_TYPE=3` 越界读。
- **JPEG 两条路都只用于整屏**（`x = y = 0`）：JPEGDEC 在 `x != 0` 时不仅裁切错位，还会把 `decoder_task` 卡死在显示路径上；局刷一律用 QOI（要正确的 JPEG 局刷则用 tjpgd）。
- `DECODER_TYPE` 是协议字段：编号由 `PUD_CMD_GET_CAPS` 上报，**不要重排**。

## 分发

```c
/* include/decoder.h，按 DECODER_TYPE 展开到 qoi_drawimg / rle_drawimg / lz4_drawimg / ... */
#define decoder_drawimg(xs, ys, xe, ye, b, l) qoi_drawimg(xs, ys, xe, ye, b, l)
```

`DECODER_TYPE=6` 额外需要"这条带落在哪个帧槽"，走另一个宏
`decoder_drawimg_slot(..., slot, serial)`；其它类型忽略后两个参数，所以它仍是宏而不是函数。

```c
/* src/decoders/decoder.c；索引即 DECODER_TYPE */
static char *decoder_names[] = { "tjpgd", "JPEGDEC", "LZ4", "QOI", "RLE",
#if PUD_INFLATE == 2
                                 "QOI+deflate (libdeflate)"
#else
                                 "QOI+deflate (tinfl)"
#endif
                                 , "QOI+deflate+dict" };
_Static_assert(DECODER_TYPE < sizeof(decoder_names) / sizeof(decoder_names[0]),
               "decoder_names[] has to cover every DECODER_TYPE");
```

> **坑**：这个数组曾漏掉 `"QOI"`，而 `DECODER_TYPE=3` 会越界读。加解码器别忘了同步它；
> `_Static_assert` 是后来补的兜底。开机串口的 `Decoder type: QOI` 就来自这里，
> 乱了说明数组有问题（见 [debugging.md](debugging.md) 的串口日志）。

## 两种 JPEG 实现（2026-09 实测，**主频 225 MHz** = board profile 1）

| | tjpgd（0） | JPEGDEC（1） |
| --- | --- | --- |
| 来源 | ChaN TJpgDec R0.03 + Bodmer 的 `swap`，vendored 在 `src/decoders/tjpgd/` | `src/decoders/jpegdec/` |
| 480×320 4:4:4 全屏（当时那张 `bootlogo.jpg`） | 114.6 ms | **76.4 ms** |
| 480×320 4:2:0 全屏（Pillow 重存） | 175.3 ms | **66.1 ms** |
| 局部刷新（`x != 0`，64×64 @ x=208） | ✓ 正确 | ✗ **坐标错并卡死显示** |
| 额外 RAM（净） | +2 KB | 0 |
| 解码栈峰值 | 600 B | 632 B |

### 与主频的关系：**实测近似完美线性**（2026-10 复量）

上面那组数字当时**没记主频** ✗（同期工程默认 profile 1 = 225 MHz，是推断不是记录）。补量：

| 时钟 | `min`（空闲态 ≈ 纯 USB，**控制量**） | `median`（稳态：解码+刷屏+USB） | fps | 判定 |
| --- | --- | --- | --- | --- |
| 225 MHz（board profile 1） | 23.25 ms | 69.04 ms | 15.06 | 设备受限 2.97x |
| 366 MHz（profile 2） | 23.10 ms | 42.59 ms | 24.23 | 设备受限 1.84x |
| **520 MHz**（pico-turbo `turbo`，VREG 1.60V，flash DIV 4 = 130 MHz） | 23.07 ms | **27.35 ms** | **37.38** | **USB 受限 1.19x** ✓ |
| 564 MHz（pico-turbo `extreme`，flash DIV 5 = 112 MHz） | — | **两次都失败**（一次应答后上负载挂住、一次连枚举都不应答） | — | **不可用** ✗ |

- 225 → 366（主频 ×1.627）稳态快 **1.621x**（差 0.4%）⇒ **JPEG 软解基本完全随主频线性**；
  225 → 520（×2.31）是 2.52x（flash 也同时从 75 提到 130 MHz）；所以这组数字**必须连主频一起引用**。
- **到 520 MHz 就不必再加了**：`steady/min` 从 2.97x 掉到 **1.19x** ⇒ 瓶颈已经变成**全速 USB 链路**
  （23 ms/帧的地板）。整屏 JPEG 在 RP2350 上的实际上限 ≈ **37 fps**，再堆主频没用 ✓。
  因此 todo #9 后半的"把解码循环放进 SRAM"**在 520 MHz 下已经没有收益空间**（那本来是为 XIP 受限的
  组合准备的）。

方法：`tools/jpeg_bench.py`（主机侧端到端，JPEG 编码在计时外）+ `median/min` 判据分离
"链路受限"与"设备受限"——**不需要挂调试器**。踩过：用 gdb/openocd 挂上去会 halt 目标，之后
设备就不再应答控制请求 ✗（是否 halt 本身引起**未定位**），而那台机器上设备没有接到调试器的
UART 桥 ⇒ 设备侧计数器也没有别的读法。想要计数器就用 `DECODER_STATS=1` 构建 + 短暂 halt 读
`g_qoi_stat_*`（JPEG 两条路的埋点已补上，见 `src/decoders/decoder_jpeg.c`）。

**对照（同一档内容、同参）：QOI 照片全屏在同样的两档下几乎不变**（139.08 → 138.75 ms，-0.2%），
因为它是**链路受限**的（131 KB/帧，`steady/min = 1.00x`）✓。这也解释了
[architecture.md](architecture.md) 里"225 MHz 只快 5%"那条 —— 那是 **QOI** 的数据，
**不能推到 JPEG 上** ✗。

**flash 分频是上高主频时第一件要算的事**：225 ↔ 366 这一对里 QSPI 与主频同比例（div 3：75 → 122 MHz，
都在 flash 的 133 MHz 额定内）⇒ XIP 里的代码也一起变快，所以"线性"在这两点间才成立 ✓。
到 520/564 就得让分频**退档**（pico-turbo 按 flash 额定自动推：本板 W25Q32 额定 133 MHz ⇒ div 4
= 130 MHz；它板文件给的默认上限只有 57 MHz ⇒ 会推成 div 10 = 52 MHz ✗，要看板子实际用什么 flash 再
用 `-DPICO_TURBO_FLASH_MAX_KHZ` 覆盖）。JPEGDEC 没有 SRAM 放置钩子（`PUD_CODEC_IN_RAM` 只覆盖
QOI/RLE ✗），所以 XIP 里的那部分在高主频下相对变慢 —— 但**实测到 520 MHz 时这条路已经撞在 USB 上**，
这件事就先不用管了 ✓。

两者的语义都是**主机把子图裁好、JPEG 自带尺寸、固件按 `(xs, ys)` 贴图**，`xe`/`ye` 不参与。

**为什么两种都留着**：JPEGDEC 在 `x != 0` 时 `iWidthUsed` 会算出负值，`draw_mcus` 于是把
`xe = x + iWidth - 1` 填成**子图内坐标**（实测 `xs=208 → xe=63`），`len` 变成巨大的无符号数；
一次这样的 flush 就足以让 `decoder_task` 卡在 `tft_video_flush` 里，帧槽永不释放、EP1 永不
重新武装（实测 `submitted/drawn = 2/0`、`s_ep1_pending_size` 一直挂着），主机只能超时。
**规范：JPEG 两条路都只用于整屏；局刷一律走 QOI。**

`draw_mcus` 里必须用 `pDraw->iWidthUsed` 而不是 `pDraw->iWidth`（后者是整图宽度：

```c
pDraw->iWidth       /* 错误：整图宽度，每行像素会偏移错位 */
pDraw->iWidthUsed   /* 正确：本次实际解码的宽度 */
```

补齐非法坐标只是防呆，**修不好 JPEG 局刷**（见 [todo.md](todo.md) 第 1 条）。

### tjpgd 侧踩过的坑

- 上游按 **8×8 块**回调，逐块 flush 会把地址窗口设 **2400 次/帧**；改成攒满 8 行再 flush
  （`TJPGD_GROUP_ROWS`，缓冲 480×8×2 = 7.5 KB）后每帧 40 次 —— 但实测只快 3~7%
  （118.5 → 111~115 ms），说明瓶颈在解码核心，不在刷屏粒度。
- **MCU 高度随色度采样变化**（4:4:4 → 8 行，4:2:0 → 16 行），一次回调可能跨 8 行组的边界，
  所以拷贝必须**按行遍历、跨组即 flush**；按"一块一次"写会按 16 行写进 8 行缓冲 ——
  480 宽的 4:2:0 图会写穿 7.5 KB。当时开机用的那张 `bootlogo.jpg` 恰好是 4:4:4，只用它测
  发现不了 —— 测这条路径要另找一张 4:2:0 的素材（现在的开机图是 PNG）。
- 净 RAM 只有 +2 KB：9.4 KB workspace（`JD_FASTDECODE 2`）+ 7.5 KB 行缓冲，替换掉了同一
  构建里 QOI 的 15 KB 乒乓缓冲（`qoi_buf_a/b`，靠 `--gc-sections` 丢弃）。

## 相关

- 类型表与各专题入口：[decoders.md](decoders.md)
- `DECODER_TYPE` 的构建开关与默认值：[../AGENTS.md](../AGENTS.md)
