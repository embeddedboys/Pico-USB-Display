# 解码器与帧流水线（索引）

> 固件的解码器在**编译期**由 `DECODER_TYPE` 选定（`CMakeLists.txt` 的 cache 变量，默认 **3 = QOI**），
> 每种类型各有一篇专题文档；编号是协议字段（`PUD_CMD_GET_CAPS` 上报），**不要重排**。

## TL;DR

- 默认 `DECODER_TYPE=3`（QOI）：桌面局刷的实测赢家，载荷最小、解码够快，链路受限时时间 ≈ 载荷 ÷ 带宽。
- `5`（QOI+deflate）与 `6`（跨帧字典）都是**实验**：`5` 主机侧会发（等级默认 6），`6` 驱动侧还不支持且存在**静默冻结**未定位。
- 改 `DECODER_TYPE` 后必须重新 `cmake`（`target_compile_definitions` 传入），并确认开机 logo 正常。
- 解码只能在 `decoder_task` 里做、EP1 流控不能去掉 —— 这两条是硬约束，见 [frame-pipeline.md](frame-pipeline.md)。

## 类型表

| 值 | 名称 | 输入 | 状态 | 详情 |
| --- | --- | --- | --- | --- |
| 0 | tjpgd | JPEG | 可用；局刷正确但慢（480×320 全屏 114.6~175.3 ms @225 MHz） | [decoder-architecture.md](decoder-architecture.md) |
| 1 | JPEGDEC | JPEG | 可用且更快（66.1~76.4 ms @225 MHz，**与主频近似线性**），但 `x != 0` 会卡死显示 | [decoder-architecture.md](decoder-architecture.md) |
| 2 | LZ4 | LZ4 band | 可用；**每个传输一个 band**（block 不能分块解码） | [lz4.md](lz4.md) |
| 3 | **QOI** | RGB565 QOI | **当前使用**（全屏比两种 JPEG 快 12~20 倍） | [qoi.md](qoi.md) |
| 4 | RLE | RGB565 RLE | 可用；高熵内容比 QOI 小，结构化内容比 QOI 大 | [codec-selection.md](codec-selection.md) |
| 5 | QOI+deflate | QOI 再套 raw deflate | **实验**；驱动还不会发，默认 `PUD_INFLATE=tinfl` | [qoiz.md](qoiz.md) |
| 6 | QOI+deflate+跨帧字典 | 同上 + 每槽字典窗口 | **实验**；驱动不支持；持续负载下会静默冻结 | [qoid.md](qoid.md) |

## 选型与主题

| 文档 | 一句话内容 |
| --- | --- |
| [decoder-architecture.md](decoder-architecture.md) | `DECODER_TYPE` 分发、`decoder_names[]`、drawimg 签名、两种 JPEG 实现、JPEGDEC 局刷 bug、**JPEG 解码与主频的关系** |
| [frame-pipeline.md](frame-pipeline.md) | 帧槽、EP1 流控（背压）、为什么解码必须在任务里、诊断计数器 |
| [qoi.md](qoi.md) | QOI 回调/非回调解码、band 乒乓、RUN 批量填充、异步刷新的缓冲区契约 |
| [codec-selection.md](codec-selection.md) | 桌面负载下 QOI/RLE/LZ4 的实测选型、同源复量、量具纠正 |

## 相关

- 协议帧格式与 EP1 流控的协议侧：[usb-protocol.md](usb-protocol.md)
- RAM 账与帧槽历史：[pitfalls-memory.md](pitfalls-memory.md)
- 调试用的计时计数器：[debugging.md](debugging.md)
