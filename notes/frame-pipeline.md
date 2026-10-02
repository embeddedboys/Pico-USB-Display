# 帧流水线与 EP1 流控

> USB 中断只把整笔 EP1 传输搬进帧槽并通知解码任务；槽满时**不武装 EP1**，由主机 bulk
> 阻塞等待（背压）。这条流控是修掉"局部刷新残影"的关键，去掉它就会静默丢帧。

## TL;DR

- 流水线：`usbd_vendor_ep1_bulk_out()`（中断）→ `decoder_submit_frame()` → `decoder_task` → TFT。
- 帧槽：`DECODER_FRAME_SLOTS = 3`，每槽 `DECODER_FRAME_MAX = PUD_MAX_TRANSFER`（RP2350 64 KB / RP2040 32 KB），有 `_Static_assert`。
- 健康判据：`g_decoder_stat_dropped == 0`，且 `drawn` 落后 `submitted` ≤ `DECODER_FRAME_SLOTS - 1`（现在 ≤2）。
- **解码绝不能在 USB 中断里做**：会因中断栈不足 HardFault（`CFSR` 的 `STKERR`）。

## 流水线

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
#define DECODER_FRAME_SLOTS 3
#define DECODER_FRAME_MAX   PUD_MAX_TRANSFER   /* RP2350 64 KB / RP2040 32 KB */

struct decoder_frame {
    u16 xs, ys, xe, ye;
    u32 size;
    u32 serial;                /* 本条带在已接受序列里的位置，只有 DECODER_TYPE 6 用 */
    u8  busy;
    u8  data[DECODER_FRAME_MAX];
};
static struct decoder_frame s_frames[DECODER_FRAME_SLOTS];
```

当前 3 槽 = RP2350 192 KB 静态 RAM。**一帧的压缩结果必须 ≤ `DECODER_FRAME_MAX`**，否则
`decoder_submit_frame()` 会计入 `g_decoder_stat_oversize` 并丢弃（超限的载荷在控制阶段
就应该被 `usb.c` 按 `EP1_RD_BUF_SIZE` 拒掉）。主机的分带机制保证单个传输不超过这个上限。

**槽位分配是"最小空闲下标 + 按下标消费"**，**不是轮转游标**。轮转版试过：它让
`DECODER_TYPE 6` 的字典槽位由构造保证，修好了 DELTA，但**在整屏负载下把流水线卡死**
（`submitted 303 / drawn 300`、`decoder_task` 阻塞在 `prvIdleTask`），已整段回退 ——
所以 type 6 现在按载荷里的序号找字典，不靠槽位算术，见 [qoid.md](qoid.md)。

**为什么 3 个槽**：2 槽时解码还露约 4 ms（整屏 77.99 ms），3 槽就完全藏进链路
（73.67 ms，与"完全不解码"的 `PUD_EP1_SINK` 版 73.64 ms 一致 ✓），tinfl/libdeflate 都一样；
代价是一座传输槽（RP2350 +64 KB、RP2040 +32 KB，两块都编得过）。**4 槽在 RP2350 上能链接
但 `.data+.bss` 到 504 KB，运行时没有栈**（实测起不来）—— 要更深的流水线得先把每次传输的
上限调小（槽随之变小，带数变多反而更利于重叠）。

### 为什么解码必须放在任务里

`usbd_vendor_ep1_bulk_out()` 运行在 **USB 中断上下文**。早期版本直接在这里调用解码，
导致 HardFault：

```
CFSR 0x8200 (STKERR + INVSTATE)
faulting PC: usbd_ep_start_read+126, r2 = <TFT 数据>
```

原因是 JPEGDEC 解码占用大量栈，USB 中断栈放不下（STKERR = 压栈失败），且耗时操作阻塞了
USB 中断。**结论：中断里只做搬运，解码一律交给 `decoder_task`。**

`decoder_task` 栈 **1024 words（4 KB）**：整条解码路径实测峰值 JPEGDEC 632 B /
tjpgd 600 B / QOI 496 B（`0xa5` 填充反推，方法见 [debugging.md](debugging.md)），
约 6 倍余量。早期给的 4096 words 是"怕 JPEGDEC 吃栈"的猜测，实测不成立 ——
JPEGDEC 的上下文在 `.bss`（`&g_jpegdec`），回调只有标量局部。

### 流控（背压）

槽满时固件**不武装 EP1**，让主机的 bulk 传输阻塞等待，而不是丢帧。实现与成立前提见
[usb-protocol.md](usb-protocol.md) 的"EP1 图像帧的处理（含流控）"：`usbd_vendor_ep1_tick()`
只在有空闲槽时武装；提交完一帧、解码任务释放槽、以及 `USBD_EVENT_CONFIGURED` 时都会调用它。

**这是修掉"局部刷新有残影"的关键改动。** 效果实测（gdb 读计数器）：

| 场景 | submitted | dropped | drawn |
| --- | --- | --- | --- |
| 桌面动画（约 5 s） | 2051 → 2772 | **0** | 2050 → 2770 |
| 桌面动画（约 1 s） | 3876 → 4593 | **0** | 3876 → 4593 |
| fbdev 压测（150×16 KB） | 7445 → 8860 | **0** | 7444 → 8860 |

上面 `drawn` 恰好落后 `submitted` 1 帧，是 2 槽时代的记录；现在是 3 槽，健康上界是 ≤2 帧。

**提交完要立刻重新武装**（`ep1_finish()` 里就调 `tick()`，不等解码任务）：载荷已经 `memcpy`
进帧槽，`ep1_read_buffer` 立刻可复用；晚一步武装时主机会先写下一帧、第一包被 NAK，而全速
总线上一次 NAK 要等一帧 —— 实测这一条值 **+17%**（桌面全屏 101 ms → 85 ms）。

## 诊断计数器

```c
volatile u32 g_decoder_stat_submitted;    /* 收到的帧数 */
volatile u32 g_decoder_stat_dropped;      /* 因无空闲槽被丢弃的帧数 */
volatile u32 g_decoder_stat_drawn;        /* 已完成绘制的帧数 */
volatile u32 g_decoder_stat_oversize;     /* 载荷装不进帧槽 / 超限 */
volatile u32 g_decoder_stat_lz4_oversize; /* LZ4: band 装不进 lz4_band[] */
volatile u32 g_decoder_stat_lz4_bad;      /* LZ4: 解码长度与窗口不符 */
volatile u32 g_decoder_stat_qoiz_bad;     /* QOIZ: inflate 失败或子头不对 */
volatile u32 g_decoder_stat_qoiz_oversize;/* QOIZ: 解出的 QOI 超过缓冲 */
volatile u32 g_decoder_stat_qoid_bad;     /* QOID: 子头坏或 tinyd 拒绝 */
volatile u32 g_decoder_stat_qoid_mismatch;/* QOID: 找不到载荷报的字典序号 */
volatile u32 g_decoder_stat_qoid_oversize;/* QOID: 条带装不进半窗 */
```

声明为 `volatile` 以便调试器读取而不被优化掉；EP1 侧还有 `g_ep1_stat`（`oversize`/`bad`/`stale`）。
用法见 [debugging.md](debugging.md)。

**只要 `dropped` 不为 0，就说明流控失效**（例如有人改回了"直接武装 EP1"），
症状就是局部刷新残影。

## 加压与断点追踪

### 触发负载

- 桌面动画本身就会产生数百帧/秒的局部刷新。
- 想手动加压：向主机的 PUD fbdev 写随机数据。**注意 fbN 编号不固定**，先确认哪个是 PUD：
  ```bash
  for f in /sys/class/graphics/fb*; do echo "$f: $(cat $f/name)"; done
  ```

### 断点追踪绘制

仓库里有现成的 gdb 脚本（`.pud-test/trace_decode.gdb`、`drawmcu.gdb` 等）用于打印每次解码的
窗口和尺寸：

```gdb
break qoi_drawimg
commands
  silent
  printf "DECODE xs=%d ys=%d xe=%d ye=%d size=%d\n", xs, ys, xe, ye, qoi_size
  continue
end
continue
```

> 断点式追踪会**严重拖慢**固件（每帧都停），只适合确认坐标是否正确，不要用来测吞吐。

## 相关

- 类型选择与 JPEG 特例：[decoder-architecture.md](decoder-architecture.md)
- 协议侧字段（header、`size` 奇偶、失效自愈）：[usb-protocol.md](usb-protocol.md)
- 帧槽与 RAM 的历史账：[pitfalls-memory.md](pitfalls-memory.md)
