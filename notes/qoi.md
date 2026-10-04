# QOI 解码路径与异步刷新

> 默认 `DECODER_TYPE=3` 用 QOI，走 **非回调 API + band 乒乓**（`QOI_NONCALLBACK=2`）：
> 解码写 buffer A 后立刻起异步刷屏、下一张解进 buffer B，解码与面板写重叠。

## TL;DR

- QOI 用非回调 API 逐矩形解码比流式回调版**每像素快约 40%**（照片 41.9 → 25.3 ms），代价是两块 band 缓冲 87348 B。
- 只对 `rect_px <= LZ4_BAND_PIXELS` 的传输走这条路；更大的矩形自动回落回调版（`qoi_buf_a/b` 8 行乒乓）。
- **缓冲区契约**：异步 flush 返回时传输仍在途，同一时刻只允许一个传输在途；缓冲复用前必须经 `tft_async_video_wait()` 或下一次 flush。
- 地址窗口等命令永远走同步路径，所以命令不会越过在途数据。

## 非回调 + band 乒乓

`rgb565_qoi` 有两个解码入口：**回调版**（流式，按批回调，缓冲可以很小）和**非回调版**
（一次解出一个矩形，要调用方给整块缓冲）。实测**回调版每个像素要多花约 40%**：它每个像素都
走"累计 + 容量检查 + 可能回调"那套宏，而非回调版就一句 `pixels[px_pos] = px`。

全屏 480×320、每档 20 张、`draw_us`（225 MHz，解码循环在 SRAM）：

| 方案 | 纯色 | 渐变 | 照片 | 噪声 |
| --- | --- | --- | --- | --- |
| A 回调版，8 行一批（原实现） | 3.85 ms | 22.2 ms | 40.6 ms | 42.9 ms |
| B 回调版，整个 band 一批 | 6.10 ms | 24.1 ms | 41.9 ms | 43.5 ms |
| C 非回调，解完再刷（无重叠） | 5.82 ms | 14.3 ms | 25.3 ms | 28.1 ms |
| **D 非回调 + band 乒乓（现在默认）** | **3.01 ms** | **11.5 ms** | **22.4 ms** | **25.0 ms** |

- **B 证明"加大批次"没用**：回调版放到整个 band 一批（一次 flush）反而丢了"解码与刷屏
  重叠"，纯色从 3.85 涨到 6.10 ms。
- **C 证明回调版本身慢**：同样"一个 band 一次 flush"，非回调版照片 41.9 → 25.3 ms。
- **D 两者兼得**：解码（非回调）写进 buffer A → 起异步 flush → 立刻返回；下一张解码进
  buffer B。下一次 flush 的窗口命令会先 `i80_finish_pending()` 等 A 传完，而那时 B 早已解完。
- **代价**：`qoi_band[2 * LZ4_BAND_PIXELS]` = 2 × 21837 × 2 B = **87348 B**（RP2350；
  旧记约 87 KB，是旧公式没减 12 B EP1 头）。原来那两块 8 行的 `qoi_buf_a/b`（15 KB）
  在默认路径上不再需要，保留给回落路径。
- **正确性**：单 band 与乒乓缓冲内容都做过逐字节对拍（43200/43200、A/B 轮转正确），连跑
  1200 帧 `drawn == submitted`、`CFSR`/`HFSR` = 0。
- **端到端也开始有收益**：桌面全屏 90.7 → **86.0 ms**、wallpaper strip 20.8 → **17.4 ms**（-16%）。
- **适用范围**：只对"矩形像素数 ≤ `LZ4_BAND_PIXELS`"的传输走这条路；更大的矩形自动回落到
  回调版。`-DQOI_NONCALLBACK=0` 可以强制用回调版做 A/B，`=1` 是"非回调但不乒乓"
  （只有测量价值，没有重叠会变慢）。**RLE 还是老的 8 行回调版**（同样改动未做，见 [todo.md](todo.md)）。

## 回调版实现（回落路径）

QOI 的**格式规范完整写在 `src/decoders/qoi/rgb565_qoi.h` 的文件头注释里**
（magic、五种 chunk、hash 函数、最坏情况大小推导）—— 这里不重复。

```c
#define QOI_BUF_ROWS 8
static uint16_t qoi_buf_a[480 * QOI_BUF_ROWS];   /* 7680 px = 15360 B */
static uint16_t qoi_buf_b[480 * QOI_BUF_ROWS];   /* 另一个做 ping-pong */

buf_cap = (size_t)width * QOI_BUF_ROWS;          /* 批次按整行对齐 */
rgb565_qoi_decompress_callback(qoi_data, qoi_size, width,
                               qoi_buf_a, qoi_buf_b, buf_cap, qoi_flush, &ctx);
```

回调把解出的批次刷到 TFT，坐标要加上子图原点：

```c
tft_video_flush(ctx->ox + xs, ctx->oy + ys,
                ctx->ox + xe, ctx->oy + ye, (void *)pixels, count * 2);
```

**坐标细节**：QOI 流内部坐标是相对于该子图的 `(0,0)`。**批次粒度按整行对齐**
（`width * QOI_BUF_ROWS`），每次回调刷的是一个矩形块而不是跨行散点，对 TFT 窗口设置更友好。

### RUN chunk 批量填充与寄存器分配教训

`rgb565_qoi_decompress_callback()` 原本逐像素迭代，`EMIT_PIXEL` 对每个像素都重做行环绕与
容量判断，RUN chunk 把同样的簿记重复了 run 次。改法：**只改 RUN 分支**，用紧凑内层循环
填完（`EMIT_RUN`）并 clamp 到图像末尾，冲刷点与原来完全一致。

| 内容（480×320 全帧） | 解码耗时 | 每像素 |
| --- | --- | --- |
| RUN 密集（纯色/渐变） | 32.65 ms → **5.0 ms** | 213 → **32.7 ns/px** |
| RGB565 密集（噪声） | — | 499 → **473 ns/px** |

**踩坑记录（很重要）**：第一版重构把 `EMIT_PIXEL` 从 5 个分支里合并成一份，结果噪声类内容
在 Cortex-M33 上**慢了 20%**（499 → 600 ns/px）。原因不是算法，而是**寄存器分配**：原版每个
分支各展开一份，`buf` 指针能驻留在寄存器；合并后它必须跨分派存活，被编译器挤到栈上
（EMIT 快路径从约 8 条指令涨到约 14 条）。所以这里刻意**保留了每个分支各展开一份的写法**。

> 教训：x86 上的 A/B（`gcc -O2/-O3`）**复现不了**这个回退（主机上反而更快）。这类寄存器
> 分配问题只能看 `arm-none-eabi-gcc` 的汇编或上机实测。

## 异步刷新

原来 `i80_write_buf_rs()` 在 `dma_channel_wait_for_finish_blocking()` 上阻塞，CPU 干等总线。
新增的异步接口把这次等待挪到**下一次调用**，中间的空档留给解码：

| 接口 | 位置 | 说明 |
| --- | --- | --- |
| `i80_write_buf_rs_async(buf, len, rs)` | `drivers/bus/pio_i80.c` | 启动 DMA 后立即返回 |
| `i80_write_sync()` | 同上 | 等待在途传输并完成尾部 CS 释放 |
| `tft_async_video_flush(...)` | `drivers/display/tft.c` | 异步版 `tft_video_flush` |
| `tft_async_video_wait()` | 同上 | 帧末等待 |

**引脚时序与同步路径完全一致**：异步版在下次调用的开头等 DMA、再释放 CS，同步版在调用内部
等 —— 只是等待的位置变了。

| 指标（全屏纯色） | 同步 | 异步 |
| --- | --- | --- |
| 帧率 | 123.8 fps（8.00 ms） | **177.5 fps（5.63 ms）** |
| partial 64×64 | 3.69 ms | **3.19 ms** |
| partial 128×64 | 6.99 ms | **6.31 ms** |

稳定性：45 秒混合重载 646 次传输（median 57.77 / p99 58.01 / max 58.46 ms，无超时）、
9000 帧高频局刷，全程 `dropped=0` 且 `drawn == submitted`。

**上限在哪**：显示路径实测 21.9 ns/px，而 PIO 理论值是 20 ns/px（`clk_div=1.5`、
`out+nop` 两周期）—— 已经跑在总线极限上，异步能拿回的只有这部分重叠收益，再往上要改的是
总线本身（提高 `TFT_BUS_CLK_KHZ` 或换总线）。

### 命令与数据的顺序（异步接口的安全前提）

- 所有命令（`set_addr_win`、`write_reg`、`tft_write_cmd/data`）都走**同步**宏
  `write_buf_dc` → `i80_write_buf_rs()`；
- 异步宏 `write_buf_dc_async` 在整库里**只有一处调用**：`tft_async_video_flush()` 里的像素数据；
- 同步入口第一步就是 `i80_finish_pending()`（等在途异步传输的 DMA 完成并释放 CS），
  再 `i80_wait_idle()` 等 PIO 排空，然后才发命令。所以命令**不可能越过**在途数据；
- 命令→数据的边界同理：`set_addr_win` 最后一个字节（`0x2C`）同步发完后，异步入口同样先
  `finish_pending()` + `wait_idle()` 才切 RS=1 启动数据 DMA。

步骤与原同步路径完全一致，只是等待发生的位置不同，因此引脚时序不变。

**测试缺口与补法（重要）**：`fps_bench.py` 的局刷用例**窗口位置是固定的**，这种情况下即使
窗口命令真的乱序、画面也看不出问题（每次重发的 CASET/RASET 值相同）。所以另有一个
**窗口逐帧移动**的测试：每帧先把上一位置擦成背景色、再画到新位置，正确时应始终只看到
一个块；任何乱序都会表现为拖影、两个块或位置错一帧。实测 807 次传输（8 段背景 + 799 个块）
精确计数、`dropped=0`、`drawn == submitted`，人眼确认"只有一个亮块在跳、无拖影"。

## 相关

- 批次乒乓开关与实测价值：[codec-selection.md](codec-selection.md)
- 流控与帧槽：[frame-pipeline.md](frame-pipeline.md)
- 非回调解码器与 `LZ4_BAND_PIXELS` 的定义：[../AGENTS.md](../AGENTS.md)
