# 踩坑合集（索引）

> 按类别拆成四篇；每条都是**实际发生过**的问题，记录"现象 → 根因 → 修法 → 验证"。

## TL;DR — 最容易踩的 5 条

1. **解码放进 USB 中断** → `CFSR` 的 `STKERR` HardFault。中断里只搬运。[并发](pitfalls-concurrency.md)
2. **去掉 EP1 流控** → 槽满静默丢帧 → 局部刷新残影。`dropped` 必须恒 0。[并发](pitfalls-concurrency.md)
3. **像素格式到处转**（RGB565/RGB666）→ 一个出处：驱动 init 的 `0x3A` COLMOD。[显示](pitfalls-display.md)
4. **直接 `mkdir build && cmake ..`** → 默认是 RP2040，烧到 Pico 2 跑不起来。[工具链](pitfalls-toolchain.md)
5. **用 `sed -i` 改 `include/bootlogo.h`** → 参数过长会把文件清空，只能 `git checkout`。[工具链](pitfalls-toolchain.md)

## 分类

| 文档 | 范围 |
| --- | --- |
| [pitfalls-concurrency.md](pitfalls-concurrency.md) | ISR 里解码、帧槽丢帧残影、超大帧卡死 |
| [pitfalls-display.md](pitfalls-display.md) | PIO+DMA 负载下卡死、LZ4 旧实现的三个问题、JPEGDEC 子图裁切、TFT 像素格式与三条写路径 |
| [pitfalls-memory.md](pitfalls-memory.md) | 静态 RAM 占用、`configTOTAL_HEAP_SIZE` 不起作用、heap 选型、RP2040 内存兼容 |
| [pitfalls-toolchain.md](pitfalls-toolchain.md) | 默认板型、`decoder_names[]`、bootlogo、WSL、子模块、CherryUSB 版本、EP1 双缓冲 |

## 相关

- 解码器与流水线的正面设计：[decoders.md](decoders.md)
- 调试方法：[debugging.md](debugging.md)
