# 解码器与帧流水线

## 解码器抽象

编译期通过 `DECODER_TYPE` 选择（`CMakeLists.txt` 第 69 行，默认 **3 = QOI**）：

| 值 | 名称 | 输入格式 | 状态 |
| --- | --- | --- | --- |
| 0 | tjpgd | JPEG | **未实现**（`decoder.h` 里直接 `#error`） |
| 1 | JPEGDEC | JPEG | 可用，但局部刷新有已知缺陷（见下） |
| 2 | LZ4 | LZ4 | 可用 |
| 3 | **QOI** | RGB565 QOI | **当前使用** |

`include/decoder.h` 用一个宏做分发：

```c
#define decoder_drawimg(xs, ys, xe, ye, b, l) qoi_drawimg(xs, ys, xe, ye, b, l)
```

调试日志里的名字来自 `decoder_names[] = { "tjpgd", "JPEGDEC", "LZ4", "QOI" }`，
开机打印 `Decoder type: QOI`。

> **坑**：这个数组曾漏掉 `"QOI"` 这一项，而 `DECODER_TYPE=3` 会越界读。
> 加解码器时别忘了同步这个数组。

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
decoder_task()  ← 独立任务，栈 4096 words
   │  take 信号量 → 找到 busy 槽
   │  mutex_enter_blocking(&decoder_mutex)
   │  decoder_drawimg(...)   ← 真正解码 + 刷 TFT
   │  mutex_exit()
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
加上调用前的 `decoder_mutex`，让刷屏与解码尽量重叠。

### 为什么从 JPEG 换成 QOI

| | JPEG (JPEGDEC) | QOI |
| --- | --- | --- |
| 有损 | 是 | **否** |
| 子图解码正确性 | MCU/crop 逻辑在 `x != 0` 时错位裁切，局部刷新不准，负载下会卡死显示 | **按像素处理，任意子矩形都正确** |
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
