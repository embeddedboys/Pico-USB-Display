# Pico-USB-Display 知识库

本目录存放固件自身的**设计说明、协议约定、解码器实现与踩坑总结**。
根目录 `README.md` 面向使用者（怎么编译、怎么连、支持哪些屏）；这里面向维护者。

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [architecture.md](architecture.md) | 启动流程、FreeRTOS 任务划分、显示与触摸配置 |
| [usb-protocol.md](usb-protocol.md) | 与主机驱动之间的 USB 厂商协议（设备侧视角） |
| [decoders.md](decoders.md) | 解码器抽象、QOI 解码、帧槽与**流控**（重点） |
| [build-and-flash.md](build-and-flash.md) | RP2350 构建、CMSIS-DAP 烧录、配置项说明 |
| [debugging.md](debugging.md) | gdb/OpenOCD 调试、HardFault 定位、解码统计计数器 |
| [scripts.md](scripts.md) | 用户空间工具：Python 脚本、C 转换器 `tools/pudcodec`、依赖选型与实测数据 |
| [pitfalls.md](pitfalls.md) | 踩坑合集：ISR 里解码、PIO/DMA 卡死、RAM 预算、bootlogo 数组 |
| [todo.md](todo.md) | **待办**：未结案的问题、RP2040 bring-up、已知待修项 |

## 相关仓库

- 主机驱动：`PUD-kernel-drivers`，其知识库在 `PUD-kernel-drivers/notes/`
  （**协议字段以那份为准**，本文档是设备侧镜像）
- 显示驱动库：`lib/pico-display-lib`（子模块）
- USB 协议栈：`lib/CherryUSB`（子模块）
- QOI 编解码库：`src/decoders/qoi/`（来自上游 `rgb565-qoi`）
- RLE 编解码库：`src/decoders/rle/`（来自上游 `rgb565-rle`）

## 维护约定

- 屏幕分辨率、引脚、缓冲区尺寸等配置分散在 `CMakeLists.txt` 与
  `lib/pico-display-lib/configs/*.cmake` 里，改动请同步本文档。
- 只写**已验证**的结论；推测显式标注。
