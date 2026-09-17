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
| `usb_task` | 256 | idle + 3 | core 0 | `usb_device_init()`，等待枚举完成后空转；USB 中断处理回调 |
| `bootlogo_task` | 256 | idle + 2 | core 1 | 开机 logo（用当前解码器格式内嵌在 `include/bootlogo.h`） |
| `indev_read` | 256 | idle + 0 | — | 触摸轮询（`INDEV_DRV_NOT_USED` 控制是否创建） |
| `decoder_task` | **4096** | idle + 1 | — | 真正的解码 + 刷屏（在 `decoder_init()` 里创建） |

`decoder_task` 的栈明显大于其它任务，因为解码过程（尤其 JPEGDEC）吃栈。

> **两核分工**：`usb_task` 绑 core 0，`bootlogo_task` 绑 core 1。
> `decoder_task` 未绑定（两个核都可能调度到它）。

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

- **`decoder_mutex`**（`mutex_t`）：保护"设置窗口 + 刷屏"这一整段，避免解码任务
  与 bootlogo 任务同时操作 TFT。
- **`s_decoder_sem`**（FreeRTOS binary semaphore）：USB 中断 → 解码任务的"有帧待处理"通知。
- **帧槽（`s_frames[]`）**：USB 回调与解码任务之间的数据交接缓冲，详见
  [decoders.md](decoders.md)。

## 与主机驱动的对应关系

| 固件 | 驱动 |
| --- | --- |
| `src/cherryusb/usbd_vendor.c` 的 `struct req_ep1_out` | `usb.c` 的 `struct req_ep1_out` |
| `struct req_ep2_in` | `pud_transfer()` 里的 4 字节请求头 |
| `DECODER_FRAME_MAX` / `EP1_RD_BUF_SIZE` | `USB_TRANS_MAX_SIZE` |
| `qoi_drawimg()` | `qoi_encode_rgb565()` |
| `REQ_*`（`usbd_vendor.h`） | `REQ_*`（`pud.h`） |
