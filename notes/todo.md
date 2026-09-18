# 待办

还没做的事，按"值得先做"排。每条都写清**为什么**、**做到哪一步了**、**怎么验证**。
做完的不在这里 —— 结论在各自的笔记里，索引见 [README.md](README.md)。

## 1. JPEGDEC 非法坐标的加固（小改动，建议做）

主机若给 `x != 0` 的矩形发 JPEG，`draw_mcus` 里 `pDraw->iWidthUsed` 会变成负值，`xe` 被算成
子图内坐标（实测 `xs=208 → xe=63`）、`len` 变成巨大的无符号数。一次这样的 flush 就把
`decoder_task` 卡死在 `tft_video_flush` 里：帧槽永不释放、EP1 永不重新武装，主机只能超时
（真机实测 `submitted/drawn = 2/0`，`s_ep1_pending_size` 一直挂着，复位才恢复）。

**做法**：`draw_mcus` 里 `iWidthUsed <= 0` 时直接跳过 flush（这帧不画），别把显示路径写坏。

**注意**：这只是防呆，**修不好** JPEG 局刷 —— 要正确的 JPEG 局刷用 tjpgd（`DECODER_TYPE=0`），
要快一律 QOI（全屏比两种 JPEG 快 12~20 倍）。详见 [decoders.md](decoders.md) 与
[pitfalls.md](pitfalls.md) 的 2.3。

## 2. bootlogo 重构带来的 +5.6%（未结案）

A/B 交替烧写、同一主机同一脚本、`full/solid（单次传输）`，两侧都稳到 ±0.02 ms：

| 固件 | 每帧 |
| --- | --- |
| HEAD（logo 由独立 `bootlogo_task` 画） | 5.20 / 5.23 ms |
| 工作区（logo 画在 `decoder_task` 里） | 5.50 / 5.52 ms |

**已用单变量实验排除**：解码/刷屏指令序列（归一化反汇编逐条相同）、`EP1_RD_BUF_SIZE`
64 ↔ 128 KB（都是 5.50 ms）、`decoder_task` 栈 1024 ↔ 4096 words（都是 5.50 ms）、
`DECODER_STATS`、核分配（两者都在 core 1）。

**剩余嫌疑**：那次重构本身（连同被删掉的 `decoder_mutex`）—— 稳态下它唯一留下的差别是
每个 `decoder_drawimg` 少了两次 mutex 进出。

**处置**：该重构与 RP2040 那批改动（协议校验、按板分缓冲、能力查询）**互相独立**，
可以单独回退换回这 5.6%。影响面只限"整屏单次传输"这一档；局刷与 DRM damage 是 USB 受限，
不受影响。完整记录见 [debugging.md](debugging.md)。

## 3. RP2040 真机 bring-up

**已验证**：`PICO_BOARD=pico` 能编译能链接，`.data/.bss` 124136 B = 256 KB 的 **47.35%**
（tjpgd 构型 48.11%）；用的是真正的 RP2040 SMP 端口（`portable/ThirdParty/GCC/RP2040/`）；
时钟按板取 **125 MHz**（RP2350 是 150）；ISR 栈落在 SCRATCH_X/Y。

**没验**：上板跑。至少要过 —— PIO 8080 总线时序、USB 枚举、面板与触摸的引脚配置
（`lib/pico-display-lib/configs/*.cmake` 里挑对应的板级配置）、实际解码性能（M0+ 比 M33 慢，
"QOI 全屏 5 ms"这个量级大概率不成立）、`PIO_USE_DMA` 在 125 MHz 下的分频是否正确。

## 4. LZ4 路径当前实际不可用

`lz4_drawimg()` 每帧 `malloc(LZ4_compressBound(480*320*2))` ≈ **308 KB**，而可用 SRAM 只剩
~238 KB —— **任何 heap 都跑不起来**（给 heap_4 静态切 320 KB 更放不下）。另外每帧 3 行
`printf`（115200 波特下约 10 ms）且是整帧解码。要修：静态 workspace 或**分带解压**
（与驱动的分带规则对齐），并去掉每帧打印。

## 5. 堆失败可见性（与 heap 选型无关）

`configUSE_MALLOC_FAILED_HOOK 0` 且没有实现 `vApplicationMallocFailedHook()`；
`xTaskCreate` 的返回值全都没检查。堆耗尽时的表现是**静默少一个任务**。
`heap_3.c` 的 `pvPortMalloc` 里本来就有调用 hook 的分支，开配置 + 实现钩子即可（打印并停下）。

## 6. RP2040 的栈溢出保护

RP2040（Cortex-M0+）**没有 PSPLIM**，任务栈溢出是静默踩内存（RP2350 上会 fault）。
建议在那边的 port 上开 `configCHECK_FOR_STACK_OVERFLOW 2`（现在两边都是 0），
或至少给关键任务留足余量。各任务实测峰值见 [debugging.md](debugging.md)。

## 7. 可选：ISR 栈保护

中断栈每核 2 KB，**没有 MSPLIM**，也没开 `PICO_USE_STACK_GUARDS`，溢出不会立刻 fault
（"不要把解码放进 USB 中断"这条规矩的根源）。`PICO_USE_STACK_GUARDS=1` 是可选做法，
**未验证**。

## 8. 零碎

- `main.c` 的 `frame_counter` 是死代码（无任何引用），可删。
- `include/pud.h` 的 `struct decoder_data { u8 type; }` 疑似孤儿（只有定义），**未核实**。
- `notes/` 里引用 `CMakeLists.txt` 时**别写行号** —— 已经漂过一次（加 tjpgd 链接行之后）。
- 提交与推送是两件事：默认**只提交、不推送**，推送需要人工放行（见仓库根 `AGENTS.md` 铁律 1）。
