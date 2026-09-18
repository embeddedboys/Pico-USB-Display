# 踩坑合集

按主题分组。每条都是**实际发生过**的问题。

---

## 一、并发与上下文

### 1.1 在 USB 中断里解码 → HardFault

**现象**：

```
CFSR = 0x8200   (STKERR + ...)
faulting PC: usbd_ep_start_read+126
r2 = <TFT 数据指针>
```

**根因**：`usbd_vendor_ep1_bulk_out()` 运行在 USB 中断上下文，早期版本直接在里面调用解码。
JPEGDEC 解码吃栈远超中断栈容量 → 压栈失败（`STKERR`）；而且耗时操作阻塞 USB 中断。

**修法**：中断里只做搬运，解码交给独立任务。

```c
void usbd_vendor_ep1_bulk_out(...)
{
    /* Do not decode here: this runs on the USB interrupt stack. */
    decoder_submit_frame(decoder_xs, decoder_ys, decoder_xe, decoder_ye,
                         ep1_read_buffer, nbytes);
}
```

`decoder_task` 的栈现在给 **1024 words（4 KB）**（其它任务 256 words / 1 KB）。
实测整条解码路径峰值 JPEGDEC 632 B、QOI 496 B，4 KB 有约 6 倍余量；
早期版本给的 4096 words 是"怕 JPEGDEC 吃栈"的猜测，实测不成立。

**排查经验**：`CFSR` 里的 `STKERR`/`MSTKERR` 基本可直接判定"某处栈不够"，
优先怀疑在中断或小栈上下文里干了重活（解码、大数组、`printf`）。

### 1.2 帧槽满时丢帧 → 局部刷新残影

**现象**：局部刷新很快，但拖动窗口/动画后屏幕上留下残影（旧内容残留）。

**根因**：只有 2 个帧槽。主机以数百帧/秒推送局部刷新时解码跟不上，
`decoder_submit_frame()` 在无空闲槽时**静默丢弃整帧**。丢掉的那帧含某区域的最新内容，
之后又被一帧**旧内容**覆盖回去 → 该区域一直保持错误的旧像素。

**修法**：**EP1 流控（背压）** —— 槽满时不武装 EP1，让主机的 bulk 传输自然阻塞。
详见 [decoders.md](decoders.md) 与 [usb-protocol.md](usb-protocol.md)。

修好后实测 `g_decoder_stat_dropped` 恒为 0，`drawn` 恰好落后 `submitted` 1 帧。

### 1.3 超大帧会卡死固件

单帧压缩结果超过 `DECODER_FRAME_MAX`（64 KB）时，`decoder_submit_frame()` 会截断，
截断的流解码失败；更糟的情况是超过 `EP1_RD_BUF_SIZE` 造成状态错乱，固件看起来"死了"。

卡死后用 gdb 复位即可恢复：

```bash
gdb-multiarch -q -nh -ex "target extended-remote localhost:3333" \
  -ex "monitor reset run" -ex "detach" -ex "quit"
```

**预防**：主机侧按像素分带（每带 ≤ 21835 像素，最坏 3 字节/像素 + 12 B EP1 header + 16 B QOI 头尾），
保证单帧永不超限。见 `PUD-kernel-drivers/notes/display-and-refresh.md`。

---

## 二、显示与外设

### 2.1 PIO + DMA 刷屏在负载下卡死

**现象**：负载上来后显示冻结，gdb 看到卡在 `dma_channel_is_busy()` 自旋循环里
（`i80_write_buf_rs`）。

**观察**：DMA 的 `TREQ_SEL` 读出来是 `0x20`，而期望值是 `1` —— PIO 与 DMA 的
DREQ 握手没有正确建立。

**当时的修法**：`CMakeLists.txt` 里关掉 DMA，走 PIO 轮询路径：

```cmake
set(PIO_USE_DMA 0)
```

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

**结论**：全刷提升 12–16%，USB 受限的用例不变（符合预期）。因此工作区里
`PIO_USE_DMA` 已改回 `1`。

> ⚠️ 仍需留意：当初的卡死是在**内核驱动 + 桌面动画**的负载下出现的，上面是用户空间流量。
> 加载驱动后建议再做一次桌面 soak 测试。若再次冻结，回退办法就是改回 `0`。

**附带推论**：全刷 153600 像素固定要 ~36 ms（≈4.3 Mpx/s），这就是全刷约 27 fps 的
天花板 —— 想继续提升要优化这条写入路径，而不是图像压缩（纯色全屏只压缩到 2.6 KB，
照样只有 27 fps）。

### 2.2 LZ4 解码器的三个问题（待修）

`lz4_drawimg()`（`src/decoders/decoder.c`）目前：

1. **每帧 `malloc`/`free` 约 307 KB 工作区**（`LZ4_compressBound(480*320*2)`）。
   在只有 512 KB SRAM 的 MCU 上这是很大的抖动源，应改成静态缓冲。
2. **每帧 3 行 `printf`**（`lz4_drawimg, size:...` 等）。115200 波特下约 10 ms/帧，
   顺手就把 LZ4 的"快"吃掉了。
3. **整帧解码**：它把整个流解到全屏工作区，再用传入的窗口去 `tft_video_flush`。
   所以**传局部窗口会用错数据** —— LZ4 路径实际上只支持整屏帧。

另外 `LZ4_decompress_safe()` 的第 4 个参数传的是 `max_compressed_size`（压缩上界），
虽然大于等于帧大小因而不出错，但语义上应是目标缓冲容量，建议改成显式的帧大小。

实测（`scripts/lz4_img_viewer.py`）：480×320 的照片类内容 LZ4 只有 **2.18:1**
（140736 B），**超过 64 KB 传输上限**，因此 LZ4 路径实际只能显示非常简单的画面。
用它之前先确认内容能压到 65535 B 以内。

### 2.3 JPEGDEC 的子图裁切

`draw_mcus` 里必须用 `pDraw->iWidthUsed`，**不是** `pDraw->iWidth`：

```c
pDraw->iWidth       /* 错误：整图宽度 */
pDraw->iWidthUsed   /* 正确：本次实际解码的宽度 */
```

用错会让每行像素偏移错位。这是最终放弃 JPEGDEC 转向 QOI 的原因之一 ——
QOI 解码器按像素处理，任意子矩形都正确，没有 MCU 对齐/裁切问题。

> **实测比“错位”更严重**：给 `x != 0` 的矩形发 JPEG 时 `iWidthUsed` 会变成负值，
> `xe` 被算成子图内坐标（实测 `xs=208 → xe=63`）、`len` 变成巨大的无符号数，
> 一次这样的 flush 就把 `decoder_task` 卡死在 `tft_video_flush` 里，帧槽永不释放、
> EP1 永不重新武装。**JPEG 只用于整屏（`x = 0`）**；要正确的 JPEG 局刷用 tjpgd，
> 要快就用 QOI。完整数据见 [decoders.md](decoders.md)。

### 2.4 TFT 层：三套写像素路径，以及"像素格式只有一个出处"（2026-09 清理）

`pico-display-lib` 的 TFT 层原来同时存在三条"把像素送到屏上"的路：

| 路径 | 谁在用 | 怎么写 |
| --- | --- | --- |
| `tft_async_video_flush()` → `write_buf_dc_async` 宏 | QOI/RLE/LZ4（默认路径） | 裸缓冲直接起 DMA |
| `tft_video_flush()` → `tftops->video_sync` | JPEGDEC/tjpgd、`PUD_DECODER_PINGPONG=0` | `set_addr_win` + `write_buf_dc` |
| 各驱动自己的 `*.video_sync` 覆盖 | 少数驱动 | 驱动自己转格式 |

外加一套 LVGL 时代的脚手架（`xToFlushQueue` + `video_flush_task` + `tft_async_video_push`
+ `tft_video_flush()` 里给自己发 `xTaskNotifyGiveIndexed` + `frame_counter`）——**全是死的**：
`main.c` 里 `xQueueCreate` 早就注释掉了（那个任务一旦真被创建会因未定义符号直接链接失败），
通知也没有任何地方 `ulTaskNotifyTake`。2026-09 把这一整套删了，`tft_video_flush()` 现在就是
一句 `video_sync` 调用；同时 `tft_probe()` 里那两个 `malloc`（64 B 寄存器缓冲 + ops 结构体）
换成 `tft_priv` 里的静态存储，`tft_clear()` 从"一个像素一次阻塞传输"改成按 480 像素一块。

**留下的规矩（重要）**：

- **像素格式（RGB565 还是 RGB666）只能有一个出处** —— 就是驱动 init 里的 `0x3A`
  （COLMOD）设置。`tft_video_sync()`/异步路径都**原样**把缓冲区送出去。
- 因此 `tft_ili9488.c` 和 `tft_ili9486.c` 里那份"RGB565→RGB666 转 3 字节/像素"的
  `video_sync` 被删掉了 —— 它们自己的 init 都是 `0x3A = 0x55`（RGB565 16-bit），
  那个转换和异步路径**互相矛盾**（异步路径是 2 字节/像素）。删掉后 SPI/I80 两条路
  都走通用的 2 B/px 路径，和 `0x55` 一致。
- **`tft_ili9481.c` 的那份保留**：它的 init 明确写 `0x3A = 0x66`（RGB666），转换是自洽的，
  而且已经接在 ops 表上。**要加回这类覆盖，必须同时改那块屏的 `0x3A`**，否则就是两套格式。
- 没有动的（有意）：`tft.c` 里 8 处 `#if TFT_BUS_TYPE` 和 header 里的 `write_buf_dc*` 宏。
  这个库被 20 来个板子配置共用，我们只能 build 验证自己的配置，把一个"编译期拼接"重构成
  总线虚表是纯好看、纯风险。

验证：异步路径逐字节对拍（`qoi_band` 双缓冲 A/B 内容与主机期望一致）；同步路径专门编了
`-DQOI_NONCALLBACK=0 -DPUD_DECODER_PINGPONG=0` 上板跑（`drawn == submitted`、无故障，
纯色档 6.98 ms/帧仍与早先"乒乓关 1.45×"的 A/B 吻合）；两条路都没有改像素字节。

---

## 三、内存

### 3.1 静态 RAM 占用：两个大块吃掉了 256 KB

用 `arm-none-eabi-size` / `nm --size-sort` 实测（当前 QOI 固件）：

| 符号 / 段 | 大小 | 说明 |
| --- | --- | --- |
| `ep1_read_buffer` | **131072 B**（128 KiB） | `EP1_RD_BUF_SIZE`，在 `.noncacheable` |
| `s_frames` | **131104 B**（128 KiB） | `DECODER_FRAME_SLOTS(2) × DECODER_FRAME_MAX(65536)` + 32 B 头，在 `.bss` |
| `qoi_buf_a` + `qoi_buf_b` | 15360 B（15 KiB） | `480 × QOI_BUF_ROWS(8) × 2 B` × 2，在 `.bss` |
| `.data` | 3648 B | 已初始化变量 |
| `.noncacheable` | 132348 B | USB 缓冲单独放在不 cache 的区 |
| `.bss` | 149804 B | 其余静态变量 |
| **data + bss 合计** | **284472 B（≈278 KiB）** | 约占 512 KB 主 SRAM 的 54% |

`size` 输出（当前固件）：

```
   text    data     bss     dec     hex  filename
  92324  132348  152124  376796  5bfdc  pico-usb-display.elf
```

链接脚本可用的主 SRAM 区间是 `0x20000000`–`0x20080000`（512 KB）。
当前 `.bss` 顶端（`end`）在 `0x20045d7c`。

**结论**：EP1 缓冲（128 KB）+ 帧槽（128 KB）= 256 KB，已经吃掉一半 SRAM。
所以**不要轻易把 `DECODER_FRAME_MAX` 或 `DECODER_FRAME_SLOTS` 翻倍** ——
改成 2 × 128 KB 就会溢出（曾评估过：约 540 KB > 512 KB）。
若确实需要更大的单帧，正确方向是：
- 保留流控（丢帧问题已经解决，"加大缓冲"不再必要）
- 或者把帧槽改成"指针 + 两级缓冲"以复用 EP1 的 128 KB

### 3.2 `configTOTAL_HEAP_SIZE` 在本项目里**不起作用**（重要）

`FreeRTOSConfig.h` 定义了：

```c
#define configTOTAL_HEAP_SIZE           (128*1024)
#define configAPPLICATION_ALLOCATED_HEAP 0
```

看起来像是"128 KB 固定堆"，但**实际链接的是 `heap_3.c`**，它的实现是直接包装
编译器的 `malloc`/`free`：

```c
/* heap_3.c */
pvReturn = malloc( xWantedSize );
```

`heap_3.c` **完全不使用 `configTOTAL_HEAP_SIZE`**（也不存在 `ucHeap` 这个符号）。
所以：

- 任务栈、TCB、队列等都由 **newlib `malloc`** 提供；
- 堆通过 `sbrk` 从 `.bss` 末尾向上增长，上限是链接脚本的 `__HeapLimit = 0x20080000`；
  **这个上限是真会被检查的**：`_sbrk` 反汇编里就是 `cmp r3, #0x20080000` /
  `movhi.w r0, #0xffffffff`，越界返回 -1 → `malloc` 返回 NULL，**不会长进中断栈**；
- 当前可用堆 = `0x20080000 - __bss_end__(0x20045de0)` ≈ **238 KB**；
- 改 `configTOTAL_HEAP_SIZE` **不会有任何效果** —— 要限制堆得改链接脚本或换 heap_4.c。

> **常见误解：heap_3 ≠ "任务栈按实际使用量分配"。**
> `xTaskCreate()` 的栈深度参数（单位是 **word**，不是 KB：256 → 1 KB）就是
> `pvPortMallocStack( 深度 × sizeof(StackType_t) )` 的实参，**创建时一次性固定分配**，
> 只有 `vTaskDelete()` 才归还。另外还会单独 `pvPortMalloc( sizeof(TCB_t) )` 一笔（128 B）。
> 那层 `0xa5` 填充（`tskSET_NEW_STACKS_TO_KNOWN_VALUE`，由 `configUSE_TRACE_FACILITY` /
> `INCLUDE_uxTaskGetStackHighWaterMark` 触发）只是**调试用的水位尺**，不参与分配，
> 也不会让栈"长大"。
>
> 反例就在实测数据里：`decoder_task` 收缩前声明 4096 words（16 KB），峰值只用了 496 B ——
> 若真按用量分配，它只该占 ~0.5 KB。所以每个任务的实际开销恒为
> `sizeof(TCB_t)`(128 B) + 深度 × 4 + malloc 块头(~8 B)，`indev_read` ≈ 1.2 KB；
> 收缩后 `decoder_task`（1024 words）≈ 4.1 KB。
>
> 顺带：heap_3 只实现 `pvPortMalloc`/`vPortFree`/`vPortHeapResetState`，
> **没有** `xPortGetFreeHeapSize()` / `xPortGetMinimumEverFreeHeapSize()`，
> 看不到 FreeRTOS 侧的堆统计；代价是每次 malloc/free 都要 `vTaskSuspendAll()`。
> `configMINIMAL_STACK_SIZE`(256) 只作用于两个 idle 任务，跟显式传的 256 无关。

想确认实际用的是哪个 heap，看链接了哪个文件即可：

```bash
grep -oE "[^ ]*heap_[0-9]\.c" build-pico2/build.ninja | sort -u
```

栈深度参数（`xTaskCreate`）以 **word** 为单位，RP2350 上 1 word = 4 B：

| 任务 | 栈参数 | 实际字节 | 实测峰值 |
| --- | --- | --- | --- |
| `usb_task` | 256 | 1 KB | 416 B |
| `indev_read` | 256 | 1 KB | 432 B |
| `decoder_task` | **1024** | **4 KB** | 496 B（QOI）/ 632 B（JPEGDEC） |
| `Tmr Svc` | 256 | 1 KB | 152 B |
| `IDLE0`/`IDLE1` | 256×2 | 1 KB×2 | 128 / 112 B |

合计约 **9 KB**（收缩前 24 KB）。`Tmr Svc` 的深度由 `configTIMER_TASK_STACK_DEPTH`
控制，在 `lib/pico-display-lib/FreeRTOSConfig.h` 里；**本工程没有创建任何软件定时器**，
所以它只是空转，256 words 足够（真要用定时器回调前先加大）。

（开机 logo 原来是独立的 `bootlogo_task`，现已并进 `decoder_task`，见
[architecture.md](architecture.md#开机-logo-不是任务)；各任务峰值的测量方法见
[debugging.md](debugging.md#任务栈水位与栈保护)。）

`xTaskCreate` 失败时返回 `errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY`，
本项目未检查该返回值 —— 堆被耗尽时会静默少一个任务。加任务/加大栈前先算总量。

### 3.3 该选 heap_几？—— 保持 `heap_3`

| 实现 | 适合本项目吗 |
| --- | --- |
| `heap_1` | ✗ 不能 `free`，`vTaskDelete` 直接不可用 |
| `heap_2` | ✗ 能 `free` 但不合并空闲块，容易碎片（已废弃） |
| **`heap_3`（当前）** | ✓ 见下 |
| `heap_4` | △ 有统计、带合并，但要把 SRAM 静态切一块，切多少得自己猜 |
| `heap_5` | ✗ 多段不连续内存才需要；RP2350 主 SRAM 是连续的 |

保住 `heap_3` 的理由：

1. **它不是"无上限"**：`_sbrk` 硬性卡在 `__HeapLimit = 0x20080000`（正好是中断栈底），
   越界是 `NULL` 而不是踩栈（上面已验证）。
2. **换 heap_4 不会凭空多出内存**，只是把同一块 SRAM 在"FreeRTOS 堆"和"newlib C 堆"
   之间**切开**：`ucHeap` 进 `.bss` 会把 `__bss_end__` 顶高，C 堆（stdio 等仍走它）相应变小。
   总额不变，却多了一个要猜的分割点。
3. 本项目堆负载很轻且集中在启动期：6 个任务的 TCB + 栈 ≈ 23 KB，其余是 newlib stdio。
   没有高频分配路径 —— 唯一的例外是 `lz4_drawimg()` 每帧
   `malloc(LZ4_compressBound(480*320*2))` ≈ **308 KB**，而**这个在任何 heap 下都跑不起来**
   （可用 SRAM 只剩 238 KB；给 heap_4 静态切 320 KB 更放不下），只能改代码
   （静态 workspace 或分带解压，AGENTS.md 已标为待修项）。heap 选择帮不上它。
4. 代价只有两条：没有 `xPortGetFreeHeapSize()` 统计；每次 malloc/free 包一层
   `vTaskSuspendAll()`（在这个分配频率下可忽略）。

**真正该补的不是换 heap，而是失败可见性**（与 heap 无关，两处改动都独立于实现）：
`configUSE_MALLOC_FAILED_HOOK 0` 且没有 `vApplicationMallocFailedHook()`；
`xTaskCreate` 返回值没检查。heap_3 的 `pvPortMalloc` 里本来就有调用 hook 的分支，
打开配置 + 实现钩子即可（打印并停下，而不是静默少一个任务）。

### 3.4 兼容 RP2040：瓶颈是那两块大缓冲，不是栈（已实测）

**先给结论**：RP2040 分支**能编译、能链接**，不需要额外的移植工作 —— 缺的只是内存。
把 `PUD_MAX_TRANSFER` 按板子分开之后，`PICO_BOARD=pico` 与 `pico2` 都能构建通过：

| 构建 | .data/.bss | 占可用 SRAM | 说明 |
| --- | --- | --- | --- |
| `pico2`（改前） | 287976 B | 55% of 512 KB | `EP1_RD_BUF_SIZE` 还是 128 KB |
| **`pico2`（现在）** | **222696 B** | 42% | EP1 缓冲 128 → 64 KB |
| `pico`（改前） | 287976 B | **109.85% of 256 KB** | **链接直接失败** |
| **`pico`（现在）** | **124136 B** | **47%** | 每帧 32 KB 上限 |

RP2040 侧剩余堆 ≈ `0x20040000 - __bss_end__(0x2001dce8)` ≈ **136 KB**，任务栈 9 KB
+ CherryUSB + stdio 之后仍然宽裕。onboard 的 ISR 栈走 SCRATCH_X/Y（`__StackTop`
= 0x20042000），不占这 256 KB。

**改了哪两处**（都在 `src/cherryusb/usbd_vendor.h` 的 `PUD_MAX_TRANSFER`）：

| 板子 | `PUD_MAX_TRANSFER` | `ep1_read_buffer` | `s_frames` | 每屏段数 |
| --- | --- | --- | --- | --- |
| RP2350 | 64 KB | 64 KB | 2 × 64 KB | 8（与改前**完全相同**） |
| RP2040 | 32 KB | 32 KB | 2 × 32 KB | ~15（480 宽 → 22 行/段） |

- 主机不再写死分带大小：驱动与 `scripts/pud_usb.py` 都用 `PUD_CMD_GET_CAPS`
  问设备（见 [usb-protocol.md](usb-protocol.md)）。RP2350 上设备报 65536，
  经 `min(65535, …)` 得到 21839 px —— **与改前的编译期常量一致，行为零变化**。
- 32 KB 是"够用且留足堆"的折中，不是硬性上限：`PUD_MAX_TRANSFER` 在
  `usbd_vendor.h` 一处定义，想给 RP2040 更大的段（更快全刷、更少堆）改一个数字即可。
- **代价在性能上，且只落在 RP2040**：分带变小 = 往返变多。实测 RP2350 上全刷纯色
  8 段的 `min`（纯 USB 下限）已经 5.7 ms ≈ **0.7 ms/段**，15 段就要 ~10 ms 的下限；
  再加上 M0+ 比 M33 慢（QOI 全刷解码 RP2350 约 5 ms，RP2040 **未实测**，预计慢数倍），
  RP2040 上全刷大概率变成"设备侧受限"。局部刷新本来就是 USB 受限，与段数无关。

> 教训：以为"要兼容小内存板子"就先动栈，其实栈只占 9 KB；真正的开销是
> `ep1_read_buffer` 与 `s_frames` 这两块，而它们的尺寸是**协议的**一部分，
> 必须让设备告诉主机，而不是两边各写一个常量。

---

## 四、工程与工具链

### 4.1 默认按 RP2040 配置（最容易踩）

项目默认 `PICO_BOARD=pico`（RP2040）。**直接 `mkdir build && cmake ..` 编出来的固件
在 Pico 2 上跑不起来。**

```bash
# build/         → PICO_BOARD=pico   / PICO_PLATFORM=rp2040     ❌
# build-pico2/   → PICO_BOARD=pico2  / PICO_PLATFORM=rp2350-arm-s ✅
cd build-pico2 && cmake .. -DPICO_BOARD=pico2 && cmake --build . -j8
```

### 4.2 `decoder_names[]` 必须包含所有类型

```c
static char *decoder_names[] = { "tjpgd", "JPEGDEC", "LZ4", "QOI", "RLE" };
```

这个数组曾漏掉 `"QOI"`，而 `DECODER_TYPE=3` 会越界读。加解码器时同步这里；
**编号不要重排**（`decoder_type` 会通过 `PUD_CMD_GET_CAPS` 上报给主机）。

### 4.3 `include/bootlogo.h` 是个超大的条件编译文件

按 `DECODER_TYPE` 分支内嵌了多份 logo 压缩数据，4500+ 行。
**不要用 `sed -i` 之类的命令式批处理去改它** —— 曾因参数列表过长导致文件被清空，
最后靠 `git checkout -- include/bootlogo.h` 才恢复。用编辑器的精确替换，或脚本内用 Python。

### 4.4 WSL 里看不到调试器和 USB 设备

WSL 没有 `/dev/bus/usb`，也无法 `mknod` 造出来。所以：

- OpenOCD 必须在 **Windows 宿主机**跑，WSL 通过 `localhost:3333` 连
- 主机侧（pyusb 之类）直接访问 Pico 在 WSL 里是做不到的
- WSL 的 `/tmp` **每次命令调用是独立的**，别把中间产物放那儿再跨调用读

### 4.5 子模块拉取

`lib/` 下是嵌套子模块（`CherryUSB`、`lz4`、`pico-display-lib`，以及 `FreeRTOS-Kernel`
和它的两个 ports 仓库）。必须 `--recursive`，漏了会在编译时报缺文件。

直连 GitHub 失败时，走代理并强制 HTTP/1.1 是验证过可行的组合：

```bash
export https_proxy=http://<proxy>:<port>
git -c http.version=HTTP/1.1 submodule update --init --recursive
```

`ghproxy`、`gitee` 镜像在本项目上验证**不可用**。

### 4.6 修改 `DECODER_TYPE` 后要重新 cmake

它是通过 `target_compile_definitions` 传进去的，只改 `.cmake` 不重新配置不会生效。
另外 `include/bootlogo.h` 的 logo 数据也随类型切换 —— 换了类型别忘了确认开机 logo 正常。

### 4.7 CherryUSB 子模块停在 v1.5.2，要不要升到 v1.6.1（2026-09 复核：不用）

我们只编译 `core/usbd_core.c` + `port/rp2040/usb_dc_rp2040.c` + 自己的 `usb.c`/`usbd_vendor.c`
（`src/cherryusb/CMakeLists.txt` 里显式列的）。**v1.5.2 → v1.6.1 一共动了 171 个文件，
但落在我们编译/包含范围内的只有 7 个**：

| 文件 | 变化 | 影响 |
| --- | --- | --- |
| `port/rp2040/usb_dc_rp2040.c` | **没变**（逐字节相同） | 无（USB 控制器行为不变） |
| `core/usbd_core.c` | 删掉非 ADVANCE_DESC 的旧路径；EP0 包长改为读设备描述符的 `bMaxPacketSize0`；`usbd_initialize/deinitialize` 清理（总线断言、EP0 mq 释放、新增 `USBD_EVENT_DEINIT`） | 我们描述符里 EP0 就是 64（宏里硬编码 `0x40`），行为一致 |
| `core/usbd_core.h` | `usbd_desc_register()` 成为唯一描述符 API（**ADVANCE_DESC 变强制**）；`usbd_initialize` 的 handler 加 typedef；多 include 两个新头 | 我们**已经**定义 `CONFIG_USBDEV_ADVANCE_DESC` ✓ |
| `common/usb_def.h` | BOS/WebUSB/WinUSB platform capability 结构体改名；注释缩进；描述符宏未动 | 我们不用这些描述符 |
| `common/usb_util.h` | `WBVAL/DBVAL` 加括号；新增 `DIV_ROUND_CLOSEST` | 更安全，无影响 |
| `common/usb_osal.h` | 多一个 `usb_osal_sem_create_counting` 原型 | 我们不用 OSAL |
| `common/usb_version.h` / `usb_otg.h` | 版本号；OTG mode 宏 | 无关 |

- 新增 `common/usb_ringbuffer.h`、`common/usb_mempool.h` 是**纯头文件**，而且 **device core 里一次都没用到**
  （`grep` = 0），`CONFIG_USB_MEMPOOL_MAX_BLOCK_COUNT` 头里自带默认值 16 → **配置不用加东西**。
- **端点路径一个字没动**：`usbd_ep_start_write/read`、`usbd_ep_close`、stall、`ep_cb`、`ep0_state`
  在 diff 里都是零行 → 我们的 EP1/EP2/EP4 与 vendor 请求处理不受影响（那边也没有当年 HardFault 的修）。
- 其余 160+ 个文件是 class/（我们不用 class，vendor class 是自己写的）、host（`usbh_*`/`usbotg_core`）、
  demo、以及 DWC2/MUSB/EHCI 的改动 —— **都不在我们编译范围内**。1.5.3/1.6.0 的更新点（serial 框架、
  HID report 解析、UVC bulk、DWC2 时钟等）也都在 host 侧。

**结论**：升级是低风险的小事，但目前**没有任何功能收益**，先不动；等上游真的加了我们要的东西
（比如 device 侧双缓冲、或某个 core 修复）再升。真要升，记得"改完必须上板跑一遍"
（171 个文件里含 `usbd_core.c` 的初始化/断言路径）。

## 五、USB 端点

### 5.1 EP1 OUT 为什么不做双缓冲（2026-09，结论：做了也没用）

RP2040/RP2350 的 USB 控制器**支持**端点双缓冲（`EP_CTRL_DOUBLE_BUFFERED_BITS` +
`EP_CTRL_INTERRUPT_PER_DOUBLE_BUFFER` + `USB_BUF_CTRL_SEL`；RP2350 上同样的位在
`usb_device_dpram.h` 里叫 `USB_DEVICE_DPRAM_EPn_IN_CONTROL_DOUBLE_BUFFERED` /
`..._INTERRUPT_PER_DOUBLE_BUFF`），但我们的 bulk OUT **用不上、也不需要**：

- **CherryUSB 的设备端没实现**：`port/rp2040/usb_dc_rp2040.c` 给每个端点方向只分一个
  64 B DPRAM 缓冲（`next_buffer_ptr += 64`），PID 自己翻。**我们锁的 v1.5.2 和上游
  `master`（v1.6.1）这个文件逐字节相同**（`diff` 无输出），所以升级子模块也不会带来它；
  只有 **host** 端 `usb_hc_rp2040.c` 实现了双缓冲。
- **参考实现也故意避开 device OUT**：pico-sdk 里的 TinyUSB `dcd_rp2040.c` 有完整双缓冲代码，
  却对 device OUT 强制单缓冲，注释是 *"skip double buffered for OUT endpoint in Device mode,
  since host could send < 64 bytes and cause short packet on buffer0"*。社区有补丁
  （notro 的 gist）把 bulk 双向都打开，但没进上游。我们的传输**每次都短包结尾**，正好在那个坑上。
- **实测它本来就不是瓶颈**：同一个 55583 B 载荷、40 帧，真 QOI **1.130 MB/s**，
  等长的垃圾数据（设备完全不解码、每包 USB 工作量相同）**1.131 MB/s** —— 一样。
  也就是说设备从来不是丢包的原因；离全速理论上限（19×64 B/ms = 1.216 MB/s）差的那 7%
  在**总线/主机侧**（每帧能排下的 packet 数、URB 结构），双缓冲救不了。
  这也和"每帧 11 KB ~ 444 KB 速率都是 1.11~1.13 MB/s 一条平线"吻合。

**结论**：继续单缓冲；链路已经是这条路的天花板，要更快只能**少发字节**（更好的压缩、更小的脏区）。
