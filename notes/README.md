# Pico-USB-Display 知识库

> 本目录是**固件自身的**设计说明、协议约定、解码器实现与踩坑总结；面向维护者。
> 根目录 `README.md` 面向使用者（怎么编译、怎么连、支持哪些屏）。通用知识库/测试约定见
> 工作区根 [`../../AGENTS.md`](../../AGENTS.md)。

**范围**：设备侧固件（RP2350/RP2040、FreeRTOS、CherryUSB、PIO 8080 TFT、编解码流水线）。
协议字段的**权威定义**在 `PUD-kernel-drivers/notes/usb-protocol.md`，本仓那几份是设备侧镜像
（`usb-protocol.md` / `usb-params.md` / `touch-ep4.md` / `reset-interface.md`）。

## 文档索引

| 分类 | 文档 | 一句话内容 |
| --- | --- | --- |
| 架构 | [architecture.md](architecture.md) | 平台/面板配置、225 MHz 与热点函数放 SRAM 的实测、启动流程、任务划分与核分配 |
| 协议 | [usb-protocol.md](usb-protocol.md) | **设备侧镜像**：端点、EP1 帧与流控/自愈、EP2 查询、caps、已知不一致 |
| 协议 | [usb-params.md](usb-params.md) | 运行期参数通道：`SET_PARAM`/`GET_PARAM`、可设字段、brightness 读回、rotation 两段式 |
| 协议 | [touch-ep4.md](touch-ep4.md) | EP4 触摸：采样旋钮、8 字节上报布局、推送/轮询、SWD 与真手指验证 |
| 协议 | [reset-interface.md](reset-interface.md) | picoboot 复位接口（接口 1）：应用态进 BOOTSEL、picotool 的过滤坑 |
| 流水线 | [frame-pipeline.md](frame-pipeline.md) | 帧槽（3 个）、EP1 背压流控、为什么解码必须在任务里、诊断计数器、加压/断点 |
| 流水线 | [decoder-architecture.md](decoder-architecture.md) | `DECODER_TYPE` 分发、`decoder_names[]`、两种 JPEG 实现与 JPEGDEC 局刷 bug |
| 编解码 | [decoders.md](decoders.md) | 解码器家族**索引**：0..6 类型表与各专题入口 |
| 编解码 | [qoi.md](qoi.md) | QOI 回调/非回调解码、band 乒乓、RUN 批量填充、异步刷新的缓冲区契约 |
| 编解码 | [lz4.md](lz4.md) | LZ4 每传输一个 band、`LZ4_BAND_PIXELS`、band 容器 logo、跨版本码流 |
| 编解码 | [qoiz.md](qoiz.md) | `DECODER_TYPE 5`：等级默认 6、量具修正、tinfl vs libdeflate |
| 编解码 | [qoid.md](qoid.md) | `DECODER_TYPE 6` 跨帧字典：tinyd、载荷收益、**静默冻结危险项**、`GET_QOID` |
| 编解码 | [codec-selection.md](codec-selection.md) | 桌面负载下 QOI/RLE/LZ4 的实测选型、同源复量、测量纠正 |
| 踩坑 | [pitfalls.md](pitfalls.md) | 踩坑合集**索引**：最容易踩的 5 条 + 分类入口 |
| 踩坑 | [pitfalls-concurrency.md](pitfalls-concurrency.md) | ISR 里解码、帧槽丢帧残影、超大帧卡死 |
| 踩坑 | [pitfalls-display.md](pitfalls-display.md) | PIO+DMA 负载下卡死、LZ4 旧实现、JPEGDEC 裁切、TFT 像素格式 |
| 踩坑 | [pitfalls-memory.md](pitfalls-memory.md) | 静态 RAM 占用、`configTOTAL_HEAP_SIZE` 不起作用、heap 选型、RP2040 内存 |
| 踩坑 | [pitfalls-toolchain.md](pitfalls-toolchain.md) | 默认板型、`decoder_names[]`、bootlogo、WSL、子模块、CherryUSB、EP1 双缓冲 |
| 调试 | [debugging.md](debugging.md) | 调试器链路、板子工作方式、HardFault 定位、任务栈水位、串口日志 |
| 调试 | [fps-bench.md](fps-bench.md) | FPS 基准读法、bootlogo +5.6% 未结案、复位后枚举、EP1 自愈验收 |
| 工具 | [scripts.md](scripts.md) | 用户空间工具与依赖、`60-` udev 规则、脚本一览、固件侧配合注意事项 |
| 工具 | [scripts-measurements.md](scripts-measurements.md) | 脚本的实测带宽、触摸数据、完整验证记录、hub vs 根口 A/B |
| 工具 | [pudcodec.md](pudcodec.md) | 离线转换器 `tools/pudcodec`、band 容器、开机 logo 生成、一致性对拍 |
| 构建 | [build-and-flash.md](build-and-flash.md) | 构建目录/`build.sh`、clangd、五种烧录方式、烧写与测量纪律、配置项 |
| 待办 | [todo.md](todo.md) | **未结案项索引**：最危险的待办 + 按主题分组的全部开放项 |
| 待办 | [ep1-payload-corruption.md](ep1-payload-corruption.md) | EP1 载荷按 64 字节粒度被破坏（未定位）的取证与下一步 |

## 维护约定

- **只写已验证的结论**；观察注明测试条件，推测显式标注"未验证"。
- 每篇首屏是 `# 标题` + `> 一句话结论` + `## TL;DR`；单篇 ≤300 行，超过就拆成一文档一问题。
- 更新既有文档用**合并后重写**，不要追加"更新 2026-xx-xx"。
- 改了行为就同步对应文档；**协议字段改动要同时改驱动仓的镜像文档**。
- 文档↔代码漂移以**工作区当前代码/配置**为准；历史上不同配置下测得的数据保留并标注条件。
- 引用 `CMakeLists.txt` 时**不要写行号**（已经漂过一次）。
