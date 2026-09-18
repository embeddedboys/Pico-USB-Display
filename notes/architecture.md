# 架构与启动流程

## 定位

固件让一块 **RP2350（Pico 2）** 充当 USB 显示器：主机把压缩后的图像流从 USB 发过来，
固件解码并通过 PIO 驱动的 8080 并口刷到 TFT 上。反向提供（目前打桩的）触摸通道。

## 平台

| 项 | 值 |
| --- | --- |
| MCU | RP2350（Cortex-M33），`PICO_BOARD=pico2` |
| RTOS | FreeRTOS（tick 1000 Hz，32 优先级；堆由 **heap_3** 包装 `malloc`，见 [pitfalls.md](pitfalls.md#32-configtotal_heap_size-在本项目里不起作用重要)） |
| USB 协议栈 | CherryUSB（`lib/CherryUSB`，device 模式，vendor class） |
| 显示库 | `lib/pico-display-lib`（子模块） |
| 图像编解码 | RGB565 QOI（`src/decoders/qoi/`） |

## 目标显示（当前生效配置）

配置来自 `CMakeLists.txt` 第 34 行 include 的
`lib/pico-display-lib/configs/pico_dm_qd3503728.cmake`：

| 项 | 值 |
| --- | --- |
| 控制器 | ILI9488（`TFT_DRV_USE_ILI9488`） |
| 总线 | 8080 并口（`TFT_BUS_TYPE 1`），16 位数据总线，走 **PIO**（`DISP_OVER_PIO 1`） |
| 面板原生分辨率 | 320 × 480（`TFT_HOR_RES` × `TFT_VER_RES`） |
| 旋转 | `TFT_ROTATION 1`（90°）→ **主机看到的是 480 × 320** |
| 关键引脚 | CS 18 / WR 19 / RS 20 / RESET 22 / BLK 28，数据总线 DB0..DB15 |
| 调试串口 | UART，115200，TX 16 / RX 17 |

### `CMakeLists.txt` 里的两处**故意覆盖**

配置 include **之后**（第 41、45 行）又强制关掉了两个功能：

```cmake
# Disable overclocking for stability (RP2350 at default 150 MHz instead of 225 MHz)
set(OVERCLOCK_ENABLED 0)

# Use the PIO polling path for the I8080 TFT writes instead of DMA
set(PIO_USE_DMA 0)
```

- `OVERCLOCK_ENABLED 0` → 按 comments 说明，跑 150 MHz 而非 225 MHz（稳定性优先）。
- `PIO_USE_DMA 0` → 走 PIO 轮询路径。**这是踩坑后的结论**：PIO + DMA 的 DREQ 交互
  在负载下会卡死（详见 [pitfalls.md](pitfalls.md)）。想改回去务必先做压力测试。

## 启动流程

```
main()                                        main.c
 ├─ 设置电压/主频（按 DEFAULT_SYS_CLK_KHZ 选 VREG 档位）
 ├─ stdio_uart_init_full()  打印 "PICO USB Display"
 ├─ pud_init()                                 src/pud.c
 │   ├─ tft_driver_init()      显示控制器初始化
 │   ├─ backlight_driver_init() 背光
 │   └─ decoder_init()          解码任务 + 互斥锁/信号量
 └─ 创建任务 → vTaskStartScheduler()
```

## 任务划分

| 任务 | 栈 | 优先级 | 核 | 职责 |
| --- | --- | --- | --- | --- |
| `usb_task` | 256 | idle + 3 | core 0（绑定） | `usb_device_init()`，等枚举完成后 **`vTaskSuspend(NULL)`**；USB 中断处理回调 |
| `indev_read` | 256 | idle + 0 | 未绑定 | 触摸轮询（`INDEV_DRV_NOT_USED` 控制是否创建），每 33 ms 一次 |
| `decoder_task` | **1024** | idle + 1 | 未绑定 | 开机 logo（只画一次，见下）+ 真正的解码 + 刷屏（在 `decoder_init()` 里创建） |
| `Tmr Svc` | 256 | `configMAX_PRIORITIES - 1` | 未绑定 | 内核软件定时器任务；**本工程没有创建任何软件定时器**，它只是空转 |

实测栈峰值（在全屏/局部/图片都跑过、触摸按下过、开机 logo 全画过一遍之后，
用 `0xa5` 填充反推；"声明栈"一列是 words，"实测峰值"是字节，方法见
[debugging.md](debugging.md#任务栈水位与栈保护)）：

| 任务 | 声明栈 | 实测峰值 | 剩余 |
| --- | --- | --- | --- |
| `decoder_task` | 1024 w（4 KB） | 496 B（QOI）/ 632 B（JPEGDEC）/ 600 B（tjpgd） | 3600 B |
| `indev_read` | 256 w（1 KB） | 432 B（含 `printf`） | 592 B |
| `usb_task` | 256 w（1 KB） | 416 B（CherryUSB 初始化） | 608 B |
| `Tmr Svc` | 256 w（1 KB） | 152 B | 872 B |
| `IDLE0` / `IDLE1` | 256 w（1 KB） | 128 B / 112 B | 896 B / 912 B |

栈总量因此从 **24 KB 降到 9 KB**（`decoder_task` 16→4 KB、`Tmr Svc` 4→1 KB），
实测性能没变（`full/solid（单次传输）` 仍是 5.00 ms、`dropped=0`）。

`decoder_task` 原来的 4096 words 是历史遗留 —— 当年怕"JPEGDEC 吃栈"，实测 JPEGDEC
整帧 480×320 只需 **632 B**：它的上下文在 `.bss`（`&g_jpegdec`），MCU 回调 `draw_mcus`
只有标量局部。缩到 4 KB 后仍有约 6 倍余量。真正吃栈的不是解码路径，而是中断上下文
（每核 2 KB，且没有栈限保护）。

任务栈有硬件栈限保护（PSPLIM = `pxStack`），溢出是 fault 不是静默踩内存；
**中断栈没有**这层保护，见 [debugging.md](debugging.md#任务栈水位与栈保护)。
**RP2040（Cortex-M0+）也没有 PSPLIM**，所以那边的余量要按"溢出即静默踩内存"来留。

### 核分配：让调度器自己分，别手工绑

`usb_task` 曾经是 `for (;;) tight_loop_contents();` —— 反汇编就是一条 `b.n` 自跳，
**不阻塞、也不带 `wfi`**。它绑在 core 0 且优先级 `idle+3`，高于 `decoder_task`
（`idle+1`），于是永远 ready 的它把 core 0 整块占死，任何更低优先级的任务都上不了核 0。

改成枚举完成后 `vTaskSuspend(NULL)`（USB 栈本身就靠中断跑，`irq_set_enabled` 是在
core 0 上做的，挂起任务不影响中断），两个核的分工立刻由 SMP 调度器自动理顺。

实测（480×320 全屏纯色，`full/solid（单次传输）` 用例，每侧 3 次 × 100 帧，
两侧都极稳 ±0.03 ms）：

| 版本 | 稳态 | 变化 |
| --- | --- | --- |
| 空转（改前） | 5.94 ms（168 fps） | — |
| **只去掉空转** | **5.06 ms（198 fps）** | **−14.8%** |
| 去掉空转 + 解码绑核 1 + indev 绑核 0 | 5.17 ms | −13.0% |

**结论：收益几乎全部来自"去掉空转"，而手工绑核反而慢约 2%。** core 0 一旦空闲，
调度器自己就会把低优先级任务搬过去；手工绑定限制了它的自由度。所以最终只做了
"挂起取代空转"，没有改任何亲和性，也没有改优先级（现有的
`decoder(1) > indev(0)`、全部低于 timer task 已经够用）。

> 教训：发现某个核"被浪费"时，先看是不是有个**永不阻塞**的任务占着它，
> 而不是急着给别的任务绑核。

### 开机 logo 不是任务

logo 曾经是一个 `bootlogo_task`（`idle+2`、绑 core 1），画完自删。它存在的原始理由是
任务体里还有 `backlight_driver_init()` —— 有些初始化得等调度器起来。**那个初始化早就
挪进 `pud_init()`（调度器之前）了**，任务体只剩"画 logo + 开背光 + 自删"，都不需要调度器。

现在这段放在 `decoder_task` 的 `for(;;)` 之前：

- 面板**只有一个写者**，所以 `decoder_mutex` 整个删掉了；
- "logo 一定是第一帧"从"靠抢锁"变成**硬保证**（以前 `decoder_task` 先创建且不绑核，
  开机瞬间如果已有主机帧排队，理论上可能先画帧再被 logo 盖住）；
- 少一个任务（TCB + 1 KB 栈）和一次 `vTaskCoreAffinitySet`。

实测开机行为（在 `backlight_set_level()` 下断点，即 logo 画完的那一刻）：
core 0 = `indev_read`、core 1 = `decoder_task` —— logo 仍在 core 1 上画，与 core 0 的
USB 初始化并行，和改前一致。改后单次传输 5.00 ms、`dropped=0`。

## 目录结构

| 路径 | 职责 |
| --- | --- |
| `main.c` | 时钟/串口初始化、任务创建 |
| `src/pud.c` | 设备抽象层：初始化序列、命令读写宏（`pud_get_ro_*` / `pud_set_rw_*`） |
| `src/cherryusb/` | USB 设备栈接入：描述符、厂商请求处理、端点回调 |
| `src/decoders/` | 解码器抽象与各实现（tjpgd / JPEGDEC / LZ4 / QOI） |
| `include/pud.h` / `include/decoder.h` | 对外接口与类型 |
| `include/bootlogo.h` | 按 `DECODER_TYPE` 分支的开机 logo 压缩数据 |
| `lib/` | 子模块：Pico SDK 之外的依赖 |

## 并发与同步

- **`s_decoder_sem`**（FreeRTOS binary semaphore）：USB 中断 → 解码任务的"有帧待处理"通知。
- **帧槽（`s_frames[]`）**：USB 回调与解码任务之间的数据交接缓冲，详见
  [decoders.md](decoders.md)。

面板**只有一个写者**（`decoder_task`，开机 logo 也在它里面画），所以不需要互斥锁 ——
曾经有过一个 `decoder_mutex`，随 `bootlogo_task` 一起删掉了。

## 与主机驱动的对应关系

| 固件 | 驱动 |
| --- | --- |
| `src/cherryusb/usbd_vendor.c` 的 `struct req_ep1_out` | `usb.c` 的 `struct req_ep1_out` |
| `struct req_ep2_in` | `pud_transfer()` 里的 4 字节请求头 |
| `DECODER_FRAME_MAX` / `EP1_RD_BUF_SIZE` | `USB_TRANS_MAX_SIZE` |
| `qoi_drawimg()` | `qoi_encode_rgb565()` |
| `REQ_*`（`usbd_vendor.h`） | `REQ_*`（`pud.h`） |
