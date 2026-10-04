# LZ4 解码路径

> LZ4 用内核自带的库，驱动不必 vendor 编码器；但它的 block 不能分块解码，所以设备
> **一次只持有一个 band**，主机必须按 `PUD_CMD_GET_CAPS` 上报的 `band_pixels` 分带。

## TL;DR

- LZ4 的每个 match 指回**同一 block** 之前产生的输出 ⇒ 整块必须落进一块连续缓冲，该缓冲同时是字典；整帧 307200 B 的 block 永远解不了。
- 设备只持有 `lz4_band[LZ4_BAND_PIXELS]` = **21837 px / 43674 B**（RP2350；公式 `((PUD_MAX_TRANSFER - 12 - 16) / 3) + 1`），一个传输一个自包含 block，协议不新增字段。
- 放不下或解码长度与窗口不符 → **计数丢弃，不截断**：`g_decoder_stat_lz4_oversize` / `g_decoder_stat_lz4_bad`。
- LZ4 码流**跨版本不保证逐字节一致**，设备只解压，两条来源的码流都在板上验证过像素精确。

## 为什么有 LZ4

Linux 内核自带 LZ4（`lib/lz4/`，`LZ4_compress_default()` 与 `LZ4_decompress_safe()` 都
`EXPORT_SYMBOL`），驱动用它就不必把 QOI/RLE 的编解码源文件 vendor 进内核。所以设备侧
这条解码路径必须真的能用。**LZ4 的价值在内核侧，不在性能**（[codec-selection.md](codec-selection.md)）。

## 旧实现为什么坏（已重写）

旧 `lz4_drawimg()` 每帧 `malloc(LZ4_compressBound(frame))` ≈ **308 KB**，在只有 512 KB SRAM
的 MCU 上永远失败（可用 SRAM 只有 ~290 KB），外加每帧 3 行 `printf`（115200 波特下约 3 ms）。
根因是**整帧解码**：它把整个流解到全屏工作区，再用传入的窗口去 `tft_video_flush`，传局部
窗口会用错数据。现在的实现见下。

## 现在的设计

设备只持有**一个 band**，不是一帧。主机按**和 QOI/RLE 完全相同**的规则分带
（`band_pixels`，来自 `PUD_CMD_GET_CAPS`），每个传输里的 LZ4 block 就是该矩形自包含的码流；
`lz4_drawimg()` 解到静态 `lz4_band[]` 里再整带刷屏。**协议没有新增字段**：band 就是这个
传输的矩形，和其它解码器一样。

```c
/* 设备侧多留 1 像素，覆盖与主机两侧取整的差异 */
#define LZ4_BAND_PIXELS \
        (((PUD_MAX_TRANSFER - PUD_EP1_HEADER_SIZE - 16) / 3) + 1)   /* RP2350: 21837 px = 43674 B */
static uint16_t lz4_band[LZ4_BAND_PIXELS];
```

> 公式里的 16 是编解码库自己的帧头/结束标记，12 是每个载荷前面的 EP1 header。旧笔记写
> 21840 px / 43680 B（以及脚本里的 43678 B），是旧公式没减那 12 B；以代码为准。

| 计数（gdb 可读） | 含义 |
| --- | --- |
| `g_decoder_stat_lz4_oversize` | band 比 `lz4_band[]` 大（主机分带太粗） |
| `g_decoder_stat_lz4_bad` | 解码长度 ≠ 窗口像素数（截断/损坏/窗口与码流不符） |

**开机 logo**：LZ4 构型不能用一个整帧 block，所以它的 logo 是**band 容器** ——
`[count][offsets][blocks]`，每个 band 一个 block，band 高度由 `height / count` 推出
（`decoder_draw_bootlogo()`）。生成方式见 [scripts.md](scripts.md)；重新生成的这一支
**同时修掉了**"LZ4 分支与其它三支不是同一张图"的老问题。

## 实测（480×320，分带，`tools/codec_compare.py`，20 帧）

| 内容 | LZ4 字节/帧 | LZ4 端到端 | QOI | RLE |
| --- | --- | --- | --- | --- |
| solid | 1297 | **7.00 ms** | 7.99 ms | 5.69 ms |
| gradient | 29971 | **30.73 ms** | 35.96 ms | 47.71 ms |
| photo | 171689 | 157.33 ms | **127.49 ms** | 206.98 ms |
| noise | 308417 | 276.40 ms | 422.85 ms | **275.74 ms** |

LZ4 的压缩率在结构化内容上明显好于 RLE、和 QOI 各有胜负，高熵内容与 RLE 相当。
代价（LZ4 构型）：静态 RAM 247276 B（QOI 构型 218944 B，多出的就是 43674 B 的 `lz4_band`，
同时少了其它解码器的乒乓缓冲）；flash 因 `-ffunction-sections` + `--gc-sections` 反而更小
（见 [build-and-flash.md](build-and-flash.md)）。

> **LZ4 码流不是跨版本逐字节一致的**：vendored 的是 liblz4 1.10.0，板子上 python-lz4
> 4.4.5 带的是 1.9.x，同一条 band 有 3/8 压缩结果不同（都是合法 block）。设备只解压
> （`LZ4_decompress_safe` 与版本无关），两条来源的码流**都在板上验证过像素精确**。

## 还没做

整带解完才刷屏，解码与面板传输没有重叠（QOI/RLE 靠乒乓重叠了）。要重叠得再加一块 band
缓冲（+43674 B）并去掉 `drawimg` 末尾的 wait，收益只在纯色这种"载荷极小、解码占比大"的
内容上，**未测**。

## 相关

- 帧槽与流控：[frame-pipeline.md](frame-pipeline.md)
- 桌面负载下的选型与同源复量：[codec-selection.md](codec-selection.md)
- band 容器生成器：[scripts.md](scripts.md)
