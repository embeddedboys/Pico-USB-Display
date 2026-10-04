# 踩坑：显示与外设

> PIO+DMA 曾在负载下卡死、后来实测更快且稳定；TFT 像素格式必须只有一个出处；
> LZ4 的旧实现（整帧 malloc + 每帧 printf）已被重写修掉。

## TL;DR

- `PIO_USE_DMA` 早期在负载下卡死（`TREQ_SEL` 读到 `0x20` 期望 `1`），重测后稳定且全刷快 12~16%，现为 `1`。
- LZ4 旧实现每帧 `malloc` ~307 KB + 3 行 `printf` + 整帧解码，**2026-09 已重写**（静态 band 缓冲、无打印、每传输一个 band）。
- JPEGDEC 在 `x != 0` 时不仅裁切错位，还会卡死显示路径；**JPEG 只用于整屏**。
- 像素格式只由驱动 init 的 `0x3A`（COLMOD）决定；`tft_video_sync()`/异步路径都原样送缓冲区，不要再"顺手转 RGB666"。

## 2.1 PIO + DMA 刷屏在负载下卡死

**现象**：负载上来后显示冻结，gdb 看到卡在 `dma_channel_is_busy()` 自旋循环里
（`i80_write_buf_rs`）。

**观察**：DMA 的 `TREQ_SEL` 读出来是 `0x20`，而期望值是 `1` —— PIO 与 DMA 的 DREQ 握手没有
正确建立。当时的修法是 `set(PIO_USE_DMA 0)`，走 PIO 轮询路径。

#### 重新启用 DMA 的实测结论（`PIO_USE_DMA=1`）

后来在用户空间用 pyusb 重测（此时固件已有 EP1 流控，帧率不再被无节制地灌满），
**DMA 路径稳定且更快**：

| 用例 | `PIO_USE_DMA=0` | `PIO_USE_DMA=1` |
| --- | --- | --- |
| full/solid（8 段） | 42.45 ms / 23.6 fps | **36.27 ms / 27.5 fps** |
| full/solid（单次传输） | 42.02 ms / 24.4 fps | **36.00 ms / 28.8 fps** |
| full/gradient（8 段） | 54.00 ms / 18.6 fps | **48.00 ms / 20.9 fps** |
| full/photo | 123.18 ms / 8.12 fps | 122.82 ms / 8.17 fps（USB 受限，不变） |
| full/noise | 427.11 ms / 2.34 fps | 426.93 ms / 2.34 fps（USB 受限，不变） |
| partial 64×64 | 2.99 ms / 320.7 fps | 3.11 ms / 322.4 fps（USB 受限，不变） |

稳定性验证（45 秒混合重载 + 9000 帧高频局刷，均为用户空间流量）：

- 混合重载 646 次传输，延迟分布极紧：median 57.79 / p99 58.15 / max 58.53 ms，无超时；
- 高频局刷 9000 帧后计数器 `submitted` 精确 +9000、`dropped=0`、`drawn == submitted`；
- 全程无 `dma_channel_is_busy` 自旋、无卡死。

**结论**：全刷提升 12~16%，USB 受限的用例不变。因此 `PIO_USE_DMA` 已改回 `1`。

> ⚠️ 仍需留意：当初的卡死是在**内核驱动 + 桌面动画**的负载下出现的，上面是用户空间流量。
> 加载驱动后建议再做一次桌面 soak 测试。若再次冻结，回退办法就是改回 `0`。

**附带推论**：全刷 153600 像素固定要 ~36 ms（≈4.3 Mpx/s），这就是全刷约 27 fps 的天花板 ——
想继续提升要优化这条写入路径，而不是图像压缩（纯色全屏只压缩到 2.6 KB，照样只有 27 fps）。

## 2.2 LZ4 旧实现的三个问题（已随重写修掉）

旧 `lz4_drawimg()`（`src/decoders/decoder.c`）曾是：

1. **每帧 `malloc`/`free` 约 307 KB 工作区**（`LZ4_compressBound(480*320*2)`），在 512 KB SRAM
   的 MCU 上是很大的抖动源，而且根本申请不到；
2. **每帧 3 行 `printf`**（`lz4_drawimg, size:...` 等），115200 波特下约 10 ms/帧，顺手就把
   LZ4 的"快"吃掉了；
3. **整帧解码**：把整个流解到全屏工作区，再用传入的窗口去 `tft_video_flush`，所以传局部窗口
   会用错数据 —— LZ4 路径实际只支持整屏帧。

另外 `LZ4_decompress_safe()` 的第 4 个参数传的是 `max_compressed_size`（压缩上界），虽然大于
等于帧大小因而不出错，但语义上应是目标缓冲容量。

**现状**：这三条都已随 2026-09 的 LZ4 重写消失（静态 band 缓冲、无 `printf`、每传输一个
band）。**可复用的教训**：大块缓冲不要每帧 `malloc`；调试打印要算代价（每帧路径上绝不留
`printf`）；"整帧缓冲"在 512 KB 的 MCU 上默认不可行。设计与实测见 [lz4.md](lz4.md)。

> 历史观测（当时条件）：`tools/img_viewer.py --codec lz4` 下 480×320 的照片类内容 LZ4 只有
> **2.18:1**（140736 B），**超过 64 KB 传输上限** —— 这正是"整帧 block"路线的死因。

## 2.3 JPEGDEC 的子图裁切

`draw_mcus` 里必须用 `pDraw->iWidthUsed`，**不是** `pDraw->iWidth`：

```c
pDraw->iWidth       /* 错误：整图宽度 */
pDraw->iWidthUsed   /* 正确：本次实际解码的宽度 */
```

用错会让每行像素偏移错位。这是最终放弃 JPEGDEC 转向 QOI 的原因之一 —— QOI 解码器按像素
处理，任意子矩形都正确，没有 MCU 对齐/裁切问题。

> **实测比"错位"更严重**：给 `x != 0` 的矩形发 JPEG 时 `iWidthUsed` 会变成负值，`xe` 被算成
> 子图内坐标（实测 `xs=208 → xe=63`）、`len` 变成巨大的无符号数，一次这样的 flush 就把
> `decoder_task` 卡死在 `tft_video_flush` 里，帧槽永不释放、EP1 永不重新武装。
> **JPEG 只用于整屏（`x = 0`）**；要正确的 JPEG 局刷用 tjpgd，要快就用 QOI。完整数据见
> [decoder-architecture.md](decoder-architecture.md)。

## 2.4 TFT 层：三套写像素路径，以及"像素格式只有一个出处"（2026-09 清理）

`pico-display-lib` 的 TFT 层原来同时存在三条"把像素送到屏上"的路：

| 路径 | 谁在用 | 怎么写 |
| --- | --- | --- |
| `tft_async_video_flush()` → `write_buf_dc_async` 宏 | QOI/RLE/LZ4（默认路径） | 裸缓冲直接起 DMA |
| `tft_video_flush()` → `tftops->video_sync` | JPEGDEC/tjpgd、`PUD_DECODER_PINGPONG=0` | `set_addr_win` + `write_buf_dc` |
| 各驱动自己的 `*.video_sync` 覆盖 | 少数驱动 | 驱动自己转格式 |

外加一套 LVGL 时代的脚手架（`xToFlushQueue` + `video_flush_task` + `tft_async_video_push` +
`tft_video_flush()` 里给自己发 `xTaskNotifyGiveIndexed` + `frame_counter`）——**全是死的**：
`main.c` 里 `xQueueCreate` 早就注释掉了（那个任务一旦真被创建会因未定义符号直接链接失败），
通知也没有任何地方 `ulTaskNotifyTake`。2026-09 把这一整套删了，`tft_video_flush()` 现在就是
一句 `video_sync` 调用；同时 `tft_probe()` 里那两个 `malloc`（64 B 寄存器缓冲 + ops 结构体）
换成 `tft_priv` 里的静态存储，`tft_clear()` 从"一个像素一次阻塞传输"改成按 480 像素一块。

**留下的规矩（重要）**：

- **像素格式（RGB565 还是 RGB666）只能有一个出处** —— 就是驱动 init 里的 `0x3A`（COLMOD）。
  `tft_video_sync()`/异步路径都**原样**把缓冲区送出去。
- 因此 `tft_ili9488.c` 和 `tft_ili9486.c` 里那份"RGB565→RGB666 转 3 字节/像素"的 `video_sync`
  被删掉了 —— 它们自己的 init 都是 `0x3A = 0x55`（RGB565 16-bit），那个转换和异步路径
  **互相矛盾**（异步路径是 2 字节/像素）。删掉后 SPI/I80 两条路都走通用的 2 B/px 路径。
- **`tft_ili9481.c` 的那份保留**：它的 init 明确写 `0x3A = 0x66`（RGB666），转换是自洽的，
  而且已经接在 ops 表上。**要加回这类覆盖，必须同时改那块屏的 `0x3A`**，否则就是两套格式。
- 没有动的（有意）：`tft.c` 里 8 处 `#if TFT_BUS_TYPE` 和 header 里的 `write_buf_dc*` 宏。
  这个库被 20 来个板子配置共用，我们只能 build 验证自己的配置，把一个"编译期拼接"重构成
  总线虚表是纯好看、纯风险。

验证：异步路径逐字节对拍（`qoi_band` 双缓冲 A/B 内容与主机期望一致）；同步路径专门编了
`-DQOI_NONCALLBACK=0 -DPUD_DECODER_PINGPONG=0` 上板跑（`drawn == submitted`、无故障，
纯色档 6.98 ms/帧仍与早先"乒乓关 1.45×"的 A/B 吻合）；两条路都没有改像素字节。

## 相关

- PIO 总线时序与异步刷新：[qoi.md](qoi.md)、[architecture.md](architecture.md)
- 内存与堆：[pitfalls-memory.md](pitfalls-memory.md)
