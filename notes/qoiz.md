# QOI + deflate（`DECODER_TYPE 5`，实验）

> 在 QOI 码流后面再加一级 raw deflate（LZ77 + Huffman），补 QOI 看不到的"像素之间"重复；
> 主机默认 deflate **level 6**，设备用 `tinfl` 或 `libdeflate` 解回 QOI 再走原 QOI 解码路径。

## TL;DR

- **收益是字节数**：level 6 下整屏少发 **26.6%** 字节，时间也少约 26%（链路受限时时间 ∝ 载荷）。
- 主机 `pud_usb.qoiz_encode()` = `qoi_encode()` + `zlib.compressobj(level, DEFLATED, -15)`；**等级默认已从 1 改成 6**。
- 设备 `qoiz_drawimg()` 解到静态 `qoiz_buf[PUD_MAX_TRANSFER]` 再交给 `qoi_drawimg()`；超限/损坏计数丢弃不截断。
- **RAM +79 KB**（`qoiz_buf` 64 KB + tinfl 8.2 KB + SRAM 代码 5 KB），RP2040 放不下；驱动仓还不会发这个码流。
- `PUD_INFLATE` 是构建开关（`tinfl` / `libdeflate`），不是协议字段；libdeflate 设备侧快 32~45%，端到端在噪声里。

## 动机与离线测算

链路已经跑满（直插根口约 1.1 MB/s），设备侧每帧只用 ~5 ms，所以能换时间的只剩**少发字节**。
QOI 只看相邻像素，**像素之间**的重复（同一个字形、同一行渐变、重复 UI 元素）它看不见；
在 QOI 码流后面加一级 LZ77 + Huffman 正好补这一块。

**离线测算**（`desktop_codecs.py` 的 8 个区域求和，按设备 `band_pixels` 分带，每带一个流）：

| 第二级 | 合成桌面 | 壁纸照片 | 主机 C 编码/8 区域 |
| --- | --- | --- | --- |
| 无（QOI） | 139430 B | 221186 B | — |
| LZ4-HC | −16.0% | −7.3% | 2.6 ms |
| **deflate 1 级** | **−29.6%** | **−25.0%** | 2.8 ms |
| deflate 6 级 | −31.1% | −25.9% | 3.3 ms |
| deflate 6 级、4 KB 窗口 | −30.7% | −25.9% | 3.1 ms |
| 原始像素直接 deflate | −26.8% | −17.8% | 10~20 ms |

1 级就拿到几乎全部收益、窗口缩到 4 KB 也不掉，而 LZ4 叠在 QOI 后面几乎没用 —— 所以选 deflate。
内核自带 `zlib_deflate`，驱动同样不必 vendor 编码器（这正是当初看中 LZ4 的理由，见 [todo.md](todo.md)）。

## 实现

主机 `pud_usb.qoiz_encode()` = `qoi_encode()` + `zlib.compressobj(1, DEFLATED, -15)`；
设备 `qoiz_drawimg()` 用 **miniz 3.0.2 的 tinfl**（`src/decoders/miniz/`，只 vendor 了 inflate
那一半，四个上游文件逐字节未改，平台设置放在我们自己的 `miniz.h` 垫片里）把传输解到静态
`qoiz_buf[PUD_MAX_TRANSFER]`，再交给 `qoi_drawimg()` —— 之后的非回调解码、band 乒乓、刷屏
全是 QOI 路径原样。一块缓冲就够：QOI 解码是同步的，`qoi_drawimg()` 返回前就读完了它（异步
flush 读的是 `qoi_band[]`）。inflate 出来的是一条 QOI band，上限就是一个传输的大小；超出或
损坏**计数丢弃、不截断**（`g_decoder_stat_qoiz_oversize` / `g_decoder_stat_qoiz_bad`）。
开机 logo 沿用 QOI 那一支（`bootlogo.h` 没有新分支）。`tinfl_decompress` 借 `MINIZ_EXPORT`
这个宏钩子放进 SRAM（`PUD_CODEC_IN_RAM`）。

**代价**：RAM **+79 KB**（312580 → 391720 B，59.6% → 74.7%）= `qoiz_buf` 64 KB +
`tinfl_decompressor` 8.2 KB（Huffman 查表）+ SRAM 里的 tinfl 代码 5 KB；flash +5.8 KB。
**RP2040 放不下**（那边只有 256 KB 可用），这条只给 RP2350。

## 量具修正（必读）

2026-09-30 那张板上 A/B 表里，**QOI 时间列不是实测** ✗ —— 它是 `desktop_codecs.py` 排序表里的
**模型**（脚本里写死的 `LINK_BYTES_PER_S = 1.1e6`，常数来自另一次会话的链路测量）。
`QOI+deflate` 那两列才是设备实测，所以那张表的 **−23% 是"模型 ÷ 实测"**，不能当时间结论引用；
**载荷列与载荷比是可信的** ✓。脚本现在带 `--device` 会先用一次真实传输**标定**并在表头写明来源
（`measured in this run` / `a model ... NOT a measurement`）。混用两列曾让"QOI 基准"被引用成
85.22 ms（真值 100.05 ms，差 15%）。

**同一把尺子重测**（Pico 2，225 MHz / flash 75 MHz，直插根口，3 帧槽；本机当天链路
**0.94 MB/s**，每一行都是设备实测）：

| 码器（整屏 480×320） | 载荷 | 时间 | 隐含速率 |
| --- | --- | --- | --- |
| QOI | 93893 B | **100.05 ms** | 0.94 MB/s |
| QOI+deflate level 6 | 68874 B | **73.67 ms** | 0.93 MB/s |
| RLE | 147492 B | 156.35 ms | 0.94 MB/s |

时间**严格正比于载荷** ✓：68874/93893 = 0.733 ⇒ 预测 73.4 ms、实测 73.67；
147492/93893 = 1.571 ⇒ 预测 157.2 ms、实测 156.35 ✓。所以正确说法是
**level 6 下 QOI+deflate 少发 26.6% 的字节，整屏就少花 26.4% 的时间** ✓；
逐区域（同一次运行）终端窗体 19.44 → 10.42 ms（−46%）、窗口空白体 2.95 → 1.50（−49%）、
面板条 2.44 → 1.48（−39%）、壁纸条 20.58 → 16.53（−20%）✓。
**绝对速率是会话属性**（早期那些 1.05~1.13 MB/s 来自另一台机/另一次量测），只有**比值**可搬。

同一次测量还改了两处默认值：

- `qoiz_encode` 默认等级 **1 → 6** ✓（等级才是主要杠杆：字节 −11.2% → −26.6%；level 9 只再多 1.6 个点）。
- **设备端 inflate 引擎不是杠杆**：3 帧槽下 tinfl 73.68 ms 对 libdeflate 73.65 ms（0.04%）✓。

## 换 inflate：libdeflate（`PUD_INFLATE`）

两种 inflate 解同一种 raw deflate，主机分不出来，所以是**构建开关，不是协议字段**：
`cmake .. -DDECODER_TYPE=5 -DPUD_INFLATE=libdeflate`（默认 `tinfl`）。libdeflate v1.26 只
vendor 了解压那一半（`src/decoders/libdeflate/`，7 个上游文件逐字节未改）。它的解压器结构体
是不透明的、靠分配钩子要内存，所以设备给它一块静态的 12 KB（`qoiz_ld_mem`，实际要 11564 B），
不走堆。它没有放置钩子（热循环是模板展开的 `static` 函数），`PUD_CODEC_IN_RAM` 下改用
**构建后 `objcopy --rename-section .text=.time_critical.libdeflate`** 把整个解压器挪进 SRAM
（`-fno-function-sections` 让它只有一个 `.text`；GNU as 没有 ELF 的 `--rename-section`）。
代价比 tinfl 多 RAM 7.7 KB（391720 → 399476 B，76.2%）、flash 5 KB。

**每 band 设备侧耗时**（同一块板，两份固件都开 `DECODER_STATS`，每档 100 个 band、band 之间
空 20 ms；`submitted == drawn`、所有丢弃计数 0、`CFSR` = 0）：

| band（inflate 后 QOI 字节） | tinfl | libdeflate | QOI 解码+刷屏 |
| --- | --- | --- | --- |
| 列表行 240×16（682 B） | 349 µs（511 ns/B） | **191 µs**（280 ns/B） | 131 µs |
| 面板条 480×24（2122 B） | 562 µs（265 ns/B） | **358 µs**（169 ns/B） | 379 µs |
| 终端窗体 288×70（~9 KB） | 1509 µs（167 ns/B） | **1022 µs**（113 ns/B） | 1306 µs |

- **libdeflate 的 inflate 快 32~45%**，小 band 上收益最大 —— 那里主要是每个流的**固定开销**
  （建 Huffman 表），而不是按字节的开销：tinfl 在 682 B 的 band 上是 511 ns/B，9 KB 时 167 ns/B。
  早期那个"174 ns/B"是大 band 为主的平均值，**不能拿来估小矩形**。
- 小 band 上 inflate 比 QOI 解码本身还贵（tinfl 2.7 倍、libdeflate 1.5 倍）。
- **但端到端没变**（整屏 65.84（tinfl）/ 66.96 ms（libdeflate）、壁纸条 17.38 / 17.17 ms，
  其余区域都在 ±0.2 ms 以内 —— **在噪声里**）。桌面负载是链路受限，设备每 band 省下的几百
  微秒藏在传输后面，和 `PUD_CODEC_IN_RAM`、批次乒乓的结论一样。
- **壁纸条的"设备追上链路"不是 inflate 造成的**：单独发这块（tinfl 固件），背靠背 17.40 ms、
  每帧前空 50 ms 则 14.07 ms（1.09 MB/s，就是链路速度）—— 确实有 ~3.3 ms 是设备侧的。可
  inflate 快了 ~0.9 ms/帧，端到端只少 0.2 ms，所以剩下那段**主要不在 inflate 里**。推测是
  照片 band 的 QOI 解码：非回调版照片内容 22.4 ms/整屏 ≈ 0.15 µs/像素，45 行的 band
  （21600 px）≈ 3.1 ms，比这个 band 的 inflate（16429 B × ~113 ns ≈ 1.9 ms）大。
  **未拆解、未验证**，要定因得在壁纸条上读 `draw_us` 与 `flush_us` 的增量。

**结论**：两种都留着，默认仍是 tinfl（小、没有构建期技巧）。libdeflate 只在**设备受限**时才
值得换：更小的 damage 矩形被批量发、链路变快（USB 高速），或 RP2040 那种更慢的核
（但 QOIZ 本来就放不进 RP2040）。

## 同源复量的位置

`DECODER_TYPE 5` 与 QOI/LZ4/RLE 在**同一回路、同一分带规则、两个内容**上的完整对比表
（真实内容整屏 QOIZ L6 100326 B / 106.85 ms 对 QOI 131183 / 138.99、LZ4 183549 / 194.23；
合成桌面 QOIZ 68874 / 74.03 对 QOI 93893 / 100.02 等）见 [codec-selection.md](codec-selection.md)。
三条结论里与本篇最相关的一条：**QOIZ level 6 在两个内容的每个区域都最小最快**。

## 没做 / 下一步

- inflate 与 QOI 解码目前**串行**；照片内容上瓶颈更像 QOI 解码而不是 inflate，所以"双核
  流水线"要先拆清壁纸条那 3.3 ms 再说。
- **驱动还不会发**：要在驱动仓里 QOI 编码后接 `zlib_deflate`（1 级、raw、`-MAX_WBITS`），
  并把 `DECODER_TYPE 5` 加进驱动的解码器表与两边的 `usb-protocol.md`。协议字段只是新增一个
  `decoder_type` 值，EP1 帧格式不变。
- 没在真实合成器的 damage 流上测（上面是 `desktop_codecs.py` 的合成桌面/截图）。

## 相关

- 帧槽与流控、`DECODER_FRAME_SLOTS` 2→3 的实测：[frame-pipeline.md](frame-pipeline.md)
- 桌面选型：[codec-selection.md](codec-selection.md)
- 协议字段：`decoder_type` 见 [usb-protocol.md](usb-protocol.md)
