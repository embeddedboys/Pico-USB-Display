# 架构与启动流程

> 固件让一块 **RP2350（Pico 2）** 充当 USB 显示器：主机把压缩图像流从 USB 发来，固件解码并
> 通过 PIO 驱动的 8080 并口刷到 TFT；反向还有一条触摸通道（EP4，设备主动推送）。

## TL;DR

- 平台：RP2350（Cortex-M33）+ FreeRTOS（tick 1000 Hz）+ CherryUSB（device、vendor class）+ `pico-display-lib`。
- 当前配置：ILI9488、8080 并口、16 位总线、PIO、原生 320×480、`TFT_ROTATION 1` → 主机看到 480×320。
- `OVERCLOCK_ENABLED=1` 走板配置 profile 1：**RP2350 225 MHz / QSPI 75 MHz / VREG 1.10V**。
- 225 MHz 只让真实负载快 5~13%（链路仍是主因）；刷屏路径被设计成**不随主频变**（PIO 时钟恒 100 MHz）。
- 面板只有一个写者（`decoder_task`），所以没有互斥锁。

## 平台

| 项 | 值 |
| --- | --- |
| MCU | RP2350（Cortex-M33），`PICO_BOARD=pico2` / `PICO_PLATFORM=rp2350-arm-s` |
| RTOS | FreeRTOS（tick 1000 Hz，32 优先级；堆由 **heap_3** 包装 `malloc`，见 [pitfalls-memory.md](pitfalls-memory.md)） |
| USB 协议栈 | CherryUSB（`lib/CherryUSB`，device 模式，vendor class） |
| 显示库 | `lib/pico-display-lib`（子模块） |
| 图像编解码 | RGB565 QOI（`src/decoders/qoi/`），类型由 `DECODER_TYPE` 选，见 [decoders.md](decoders.md) |

## 目标显示（当前生效配置）

配置来自仓库自己的 `configs/pico_dm_qd3503728.cmake`（由 `-DPUD_CONFIG` 选中）：

| 项 | 值 |
| --- | --- |
| 控制器 | ILI9488（`TFT_DRV_USE_ILI9488`） |
| 总线 | 8080 并口（`TFT_BUS_TYPE 1`），16 位数据总线，走 **PIO**（`DISP_OVER_PIO 1`） |
| 面板原生分辨率 | 320 × 480（`TFT_HOR_RES` × `TFT_VER_RES`） |
| 旋转 | `TFT_ROTATION 1`（90°）→ **主机看到的是 480 × 320** |
| 关键引脚 | CS 18 / WR 19 / RS 20 / RESET 22 / BLK 28，数据总线 DB0..DB15 |
| 调试串口 | UART，115200，TX 16 / RX 17 |

`CMakeLists.txt` 在 include 配置**之后**按本项目需要覆盖两项：

```cmake
# 跑板子配置里的 overclock profile 1：RP2350 225 MHz / QSPI 75 MHz / 默认 1.10V
set(OVERCLOCK_ENABLED 1)

# 用 PIO + DMA 路径写 I8080 TFT
set(PIO_USE_DMA 1)
```

- `OVERCLOCK_ENABLED 1` → profile 1（RP2350 **225 MHz**、QSPI **75 MHz**、VREG 默认 1.10V）。
  `SYS_CLK_KHZ` / `PERI_CLK_KHZ` 由 `drivers/clk/config.cmake` 按 profile 给出，主频由 `main.c`
  的 `set_sys_clock_khz()` 设置。**注意 QSPI 分频只编译进 boot stage 2**（`PICO_FLASH_SPI_CLKDIV`），
  它在 `main()` 抬主频**之前**就生效、之后一直不变，所以换主频必须同时确认这个分频值。
- `PIO_USE_DMA 1` 是踩坑后的结论：早期 PIO 与 DMA 的 DREQ 交互在负载下卡死，重测通过后才
  改回 1，见 [pitfalls-display.md](pitfalls-display.md)。

### 主频 225 MHz（2026-09 实测）

| 用例 | 150 MHz | 225 MHz |
| --- | --- | --- |
| 6000 × 32×32 小矩形连发（gap 0） | 2417 rect/s | **2585 rect/s**（+7%） |
| 2000 × 200×200 纯色矩形（载荷很小） | 158 rect/s | 159 rect/s（**没变**） |
| 桌面负载全屏（`desktop_codecs --frames 150`，QOI） | 94.00 ms | **89.10 ms**（-5%） |
| 同上 wallpaper strip | 23.79 ms | **20.79 ms**（-13%） |

**别把这 5% 外推到 JPEG**：那几档都是 **QOI**，而 QOI 全屏桌面是**链路受限**（93 893 B ÷ ~1 MB/s
= 89 ms）⇒ 加主频当然没用 ✓。JPEG 载荷小得多（整屏 ~20 KB）、**设备侧解码受限**，实测
225 → 366 MHz（×1.627）时稳态从 69.04 → **42.59 ms（×1.62，近似完全线性）**，
见 [decoder-architecture.md](decoder-architecture.md)。

**为什么只快这么一点**（`DECODER_STATS=1` 实测拆解）：

| 用例 | 设备侧 `draw_us` | 其中 `flush_us` | 像素 |
| --- | --- | --- | --- |
| 200×200 纯色（2 band） | **1065 µs** | 122 µs（26 次 flush） | 40000 |
| 480×320 纯色（8 band） | **3968 µs** | 332 µs（43 次 flush） | 153600 |

刷屏本身不慢，但**解码要看内容**（全屏 480×320、每档 20 帧的平均，`DECODER_STATS`）：

| 内容 | 设备侧 `draw_us`/帧 | 折合 µs/像素 | 主要花在哪 |
| --- | --- | --- | --- |
| 纯色 | 4.14 ms | 0.027 | 刷屏（其中 PIO 转移就有 3.07 ms） |
| 渐变 | 24.1 ms | 0.157 | 解码 |
| 照片 | 44.5 ms | 0.290 | 解码 |
| 噪声 | 45.6 ms | 0.297 | 解码 |

所以"0.026 µs/像素"只对**低熵内容**成立；真实的照片/噪声内容是**解码主导**（≈65 周期/像素
@225 MHz）。**刷屏那条路径仍然不是瓶颈**：

- i80 PIO 程序只有两条指令（`out pins,16 side 0` + `nop side 1`）＝**每个 16 位写占 2 个 PIO
  周期**；`clkdiv = PERI/2/TFT_BUS_CLK` = 2.25（板上 `PIO0 SM0_CLKDIV` 读到 `0x00024000`），
  PIO 时钟 100 MHz → **50 M 次写/s（0.02 µs/像素、100 MB/s）**。150 MHz 时 clkdiv 1.5、
  225 MHz 时 2.25，两边都是 100 MHz —— **面板速率本来就是设计成不随主频变的**。实测
  0.024 µs/像素（`draw_us − flush_us`）已是理论值的 ~85%。
- 所以 200×200 那档"没变"不是刷屏受限，而是 **`tightloop.py` 量的是宿主机**：同一个
  480×45（21600 像素）band，宿主机花 **2.72 ms 在 Python QOI 编码器**上、0.14 ms 在 EP0 窗口、
  0.48 ms 在 bulk 写，而设备只要 0.56 ms。**别拿 `tightloop.py` 的 rect/s 当设备性能。**
- 真实负载的瓶颈是**全速 USB 链路（约 1 MB/s）**：全屏桌面内容 93893 B 要传 89 ms，而设备侧
  同一帧按内容要 4~45 ms —— 链路仍是主因，但设备侧已经不是零头了。想更快首先得减少字节数
  （更好的压缩、更小的脏区），不是加主频、也不是改 PIO。

### 更高主频：接 `pico-turbo`（2026-10 实测）

`lib/pico-turbo` 本来就是本仓的子模块（`.gitmodules`）。接法：`add_subdirectory()` + 链接
`pico_turbo`，在 `main()` 里**时钟块之后**调一次 `pico_turbo_init()`。**不指定目标频率时它是
no-op**（configure 会打 `pico-turbo: disabled (stock clocks)`）⇒ 接进来本身不改变现有行为 ✓。

实测（同一份 `DECODER_TYPE=1` 固件、整屏 480×320 JPEG，见
[decoder-architecture.md](decoder-architecture.md)）：**520 MHz（`turbo`，1.60V）可用**，
整屏 JPEG 从 15.1 → **37.4 fps**（此时已 USB 受限）；**564 MHz（`extreme`）不可用** ✗
（两次都失败：一次应答 `caps` 后上负载挂住、一次连枚举都不应答）。后者与 pico-turbo 板文件里
"564 是**单核**档、双核会 hard-fault"的独立观察一致 —— 本仓是双核 FreeRTOS 固件 ✓。

两个必须知道的坑（都是实测）：

1. **它会顺手把 `clk_peri` 重指到 `clk_sys`，而本仓的 PIO 分频是拿编译期常量算的**
   （`drivers/bus/pio_i80.c`：`DEFAULT_PIO_CLK_KHZ / 2 / TFT_BUS_CLK_KHZ`，其中
   `DEFAULT_PIO_CLK_KHZ = 构建时的 PERI_CLK_KHZ`）⇒ 若不处理，面板时钟会跟着涨到
   520/2.25 ≈ **231 MHz**，远超 ILI9488 的极限 ✗。所以 `pico_turbo_init()` 之后要把
   `clk_peri` 恢复成编译时的值（`main.c` 里就照原样再 `clock_configure` 一次）。
   pico-turbo 自己的头文件也警告过这类"把频率烘进编译期常量"的应用。
2. **`-DPICO_TURBO_PROFILE=none` 在 pico2 上会报 unknown profile** ✗（`docs/configuration.md`
   说 `none` = stock，但板文件 `boards/pico2.cmake` 不接受它）⇒ 想要 stock 就**别传这个变量**
   （留空 = no-op ✓），或传空串 `-DPICO_TURBO_PROFILE=`。

### 热点函数放 SRAM（`PUD_CODEC_IN_RAM`，2026-09 实测）

QOI/RLE 的解码循环放进 SRAM 后（QOI 段 2.3 KB、RLE 段 1.8 KB），设备侧解码快 2~9%
（全屏 480×320、每档 20 帧的平均）：

| 内容 | QOI 全在 flash | QOI 解码在 SRAM | RLE 全在 flash | RLE 解码在 SRAM |
| --- | --- | --- | --- | --- |
| 纯色 | 4.14 ms | **3.85 ms** | 3.64 ms | **3.55 ms** |
| 渐变 | 24.1 ms | **22.2 ms** | 9.39 ms | **8.67 ms** |
| 照片 | 44.5 ms | **40.6 ms** | 16.7 ms | **15.3 ms** |
| 噪声 | 45.6 ms | **42.9 ms** | 3.91 ms | **3.76 ms** |

- **只标记热点函数就够了**：QOI 那 2.3 KB 的收益和整份固件搬进 SRAM（`copy_to_ram`，+83 KB RAM）
  几乎一样，所以不需要 `copy_to_ram`。
- **端到端基本看不出来**（桌面负载链路受限：QOI 照片 120.7→118.8 ms、RLE 照片 197.7→198.0 ms），
  只有**载荷很小、设备受限**的用例有收益（QOI 纯色 4.97→4.57 ms/帧，-8%）。值不值得留取决于
  以后是否换到**设备受限**的场景（JPEG 解码、或链路变快以后）。
- **实现**（默认开，`cmake .. -DPUD_CODEC_IN_RAM=0` 关掉）：两个编解码库各有一个放置钩子，
  上游 `rgb565-qoi` / `rgb565-rle` 的 `RGB565_QOI_SECTION` / `RGB565_RLE_SECTION`（默认空），
  本项目在 `src/decoders/{qoi,rle}/CMakeLists.txt` 里传
  `-DRGB565_*_SECTION=__attribute__((section(".time_critical.*")))`。这样 **vendored 文件仍与上游
  逐字节一致**，不必为了放 RAM 去改 vendored 代码。副作用：调用方在 flash、被调方在 SRAM，
  会各多一条 linker veneer —— 每帧一次，可忽略。

稳定性：225 MHz 下 6000 × 32×32 + 2000 × 200×200 连发 `errors=0`、`drawn == submitted`、
`CFSR`/`HFSR` 保持 0。板上核对过：`PLL_SYS` FBDIV=75、REFDIV=1、postdiv 4/1 → 225 MHz，
`QMI_M0_TIMING` 的 CLKDIV=3 → QSPI 75 MHz。

## 启动流程

```
main()                                        main.c
 ├─ 设置电压/主频（按 DEFAULT_SYS_CLK_KHZ 选 VREG 档位）
 ├─ stdio_uart_init_full()  打印 "PICO USB Display"
 ├─ pud_init()                                 src/pud.c
 │   ├─ tft_driver_init()      显示控制器初始化
 │   ├─ backlight_driver_init() 背光
 │   └─ decoder_init()          解码任务 + 信号量
 └─ 创建任务 → vTaskStartScheduler()
```

## 任务划分

| 任务 | 栈 | 优先级 | 核 | 职责 |
| --- | --- | --- | --- | --- |
| `usb_task` | 256 | idle + 3 | core 0（绑定） | `usb_device_init()`，等枚举完成后 **`vTaskSuspend(NULL)`**；USB 中断处理回调 |
| `indev_read` | 256 | idle + 0 | 未绑定 | 触摸轮询（`INDEV_DRV_NOT_USED` 控制是否创建），每 10 ms 一次，采样经 EP4 推给主机 |
| `decoder_task` | **1024** | idle + 1 | 未绑定 | 开机 logo（只画一次）+ 真正的解码 + 刷屏（在 `decoder_init()` 里创建） |
| `Tmr Svc` | 256 | `configMAX_PRIORITIES - 1` | 未绑定 | 内核软件定时器任务；**本工程没有创建任何软件定时器**，它只是空转 |

实测栈峰值（全屏/局部/图片都跑过、触摸按下过、开机 logo 画过之后，用 `0xa5` 填充反推；
"声明栈"是 words，"实测峰值"是字节，方法见 [debugging.md](debugging.md)）：

| 任务 | 声明栈 | 实测峰值 | 剩余 |
| --- | --- | --- | --- |
| `decoder_task` | 1024 w（4 KB） | 496 B（QOI）/ 632 B（JPEGDEC）/ 600 B（tjpgd） | 3600 B |
| `indev_read` | 256 w（1 KB） | 432 B（含已删除的 `printf`，未重测） | 592 B |
| `usb_task` | 256 w（1 KB） | 416 B（CherryUSB 初始化） | 608 B |
| `Tmr Svc` | 256 w（1 KB） | 152 B | 872 B |
| `IDLE0` / `IDLE1` | 256 w（1 KB） | 128 B / 112 B | 896 B / 912 B |

栈总量因此从 **24 KB 降到 9 KB**（`decoder_task` 16→4 KB、`Tmr Svc` 4→1 KB），实测性能没变
（`full/solid（单次传输）` 仍是 5.00 ms、`dropped=0`）。

`decoder_task` 原来的 4096 words 是历史遗留 —— 当年怕"JPEGDEC 吃栈"，实测 JPEGDEC 整帧
480×320 只需 **632 B**：它的上下文在 `.bss`（`&g_jpegdec`），回调只有标量局部。真正吃栈的
不是解码路径，而是中断上下文（每核 2 KB，且没有栈限保护）。

任务栈有硬件栈限保护（PSPLIM = `pxStack`），溢出是 fault 不是静默踩内存；**中断栈没有**这层
保护。**RP2040（Cortex-M0+）也没有 PSPLIM**，所以那边的余量要按"溢出即静默踩内存"来留。

### 核分配：让调度器自己分，别手工绑

`usb_task` 曾经是 `for (;;) tight_loop_contents();` —— 反汇编就是一条 `b.n` 自跳，
**不阻塞、也不带 `wfi`**。它绑在 core 0 且优先级 `idle+3`，高于 `decoder_task`（`idle+1`），
于是永远 ready 的它把 core 0 整块占死。改成枚举完成后 `vTaskSuspend(NULL)`（USB 栈本身就靠
中断跑，`irq_set_enabled` 是在 core 0 上做的，挂起任务不影响中断），两个核的分工立刻由 SMP
调度器自动理顺。

实测（480×320 全屏纯色，`full/solid（单次传输）` 用例，每侧 3 次 × 100 帧，两侧都极稳 ±0.03 ms）：

| 版本 | 稳态 | 变化 |
| --- | --- | --- |
| 空转（改前） | 5.94 ms（168 fps） | — |
| **只去掉空转** | **5.06 ms（198 fps）** | **−14.8%** |
| 去掉空转 + 解码绑核 1 + indev 绑核 0 | 5.17 ms | −13.0% |

**结论：收益几乎全部来自"去掉空转"，而手工绑核反而慢约 2%。** core 0 一旦空闲，调度器自己
就会把低优先级任务搬过去；手工绑定限制了它的自由度。所以最终只做了"挂起取代空转"。

> 教训：发现某个核"被浪费"时，先看是不是有个**永不阻塞**的任务占着它，而不是急着给别的任务绑核。

### 开机 logo 不是任务

logo 曾经是一个 `bootlogo_task`（`idle+2`、绑 core 1），画完自删。它存在的原始理由是任务体里
还有 `backlight_driver_init()` —— 有些初始化得等调度器起来。**那个初始化早就挪进 `pud_init()`
（调度器之前）了**，任务体只剩"画 logo + 开背光 + 自删"，都不需要调度器。

现在这段放在 `decoder_task` 的 `for(;;)` 之前：

- 面板**只有一个写者**，所以 `decoder_mutex` 整个删掉了；
- "logo 一定是第一帧"从"靠抢锁"变成**硬保证**；
- 少一个任务（TCB + 1 KB 栈）和一次 `vTaskCoreAffinitySet`。

实测开机行为（在 `backlight_set_level()` 下断点，即 logo 画完的那一刻）：core 0 = `indev_read`、
core 1 = `decoder_task` —— logo 仍在 core 1 上画，与 core 0 的 USB 初始化并行，和改前一致。
改后单次传输 5.00 ms、`dropped=0`。（这次重构带来 +5.6% 的未结案偏差，见 [fps-bench.md](fps-bench.md)。）

## 目录结构

| 路径 | 职责 |
| --- | --- |
| `main.c` | 时钟/串口初始化、任务创建 |
| `src/pud.c` | 设备抽象层：初始化序列、命令读写宏（`pud_get_ro_*` / `pud_set_rw_*`） |
| `src/cherryusb/` | USB 设备栈接入：描述符、厂商请求处理、端点回调 |
| `src/decoders/` | 解码器抽象与各实现（tjpgd / JPEGDEC / LZ4 / QOI / RLE / QOIZ / QOID） |
| `include/pud.h` / `include/decoder.h` | 对外接口与类型（含协议结构体） |
| `include/bootlogo.h` | 按 `DECODER_TYPE` 分支的开机 logo 压缩数据（生成文件） |
| `tools/` | 主机侧工具与离线转换器 `pudcodec`（不参与固件构建） |
| `lib/` | 子模块：Pico SDK 之外的依赖 |

## 并发与同步

- **`s_decoder_sem`**（FreeRTOS binary semaphore）：USB 中断 → 解码任务的"有帧待处理"通知。
- **帧槽（`s_frames[]`）**：USB 回调与解码任务之间的数据交接缓冲，详见 [frame-pipeline.md](frame-pipeline.md)。
- **EP1 流控**取代了丢帧：槽满时不武装 EP1，主机阻塞等待。

面板**只有一个写者**（`decoder_task`，开机 logo 也在它里面画），所以不需要互斥锁 —— 曾经有过
一个 `decoder_mutex`，随 `bootlogo_task` 一起删掉了。

## 与主机驱动的对应关系

| 固件 | 驱动 |
| --- | --- |
| `include/pud.h` 的 `struct pud_ep1_header` | `pud.h` 的 `struct pud_ep1_header` |
| `struct req_ep2_in` | `pud_transfer()` 里的 4 字节请求头 |
| `DECODER_FRAME_MAX` / `EP1_RD_BUF_SIZE` | `USB_TRANS_MAX_SIZE` |
| `struct pud_caps` | 能力查询的应答结构 |
| `qoi_drawimg()` | `qoi_encode_rgb565()` |
| `REQ_*`（`usbd_vendor.h`） | `REQ_*`（`pud.h`） |

## 相关

- 编译期开关与默认值：[../AGENTS.md](../AGENTS.md)
- 协议设备侧镜像：[usb-protocol.md](usb-protocol.md)
- 解码器：[decoders.md](decoders.md)
