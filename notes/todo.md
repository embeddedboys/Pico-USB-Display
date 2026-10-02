# 待办（未结案项索引）

> 还没做的事。每条写清**为什么 / 做到哪一步 / 怎么验证**；做完的结论在各自笔记里，
> 细节尽量链到专题文档，本文只保留状态与入口。

## TL;DR — 最危险的待办

1. **EP1 载荷在持续负载下按 64 字节粒度被破坏（未定位）** —— 所有解码器都走这条重组路径，
   目前只有 `DECODER_TYPE 6` 会把它计数出来；若被破坏的载荷恰好通过帧头校验，注入的会是
   一帧**错误画面**而不是被丢弃的一帧。见 [ep1-payload-corruption.md](ep1-payload-corruption.md)。
2. **`DECODER_TYPE 6` 在持续负载下会静默冻结**（`submitted` 继续涨、`drawn` 停住、
   `dropped == 0`，没有任何计数报警）；约 15,000 次发送未能复现。**合入默认配置的前提是
   定位根因或补看门狗**。见 [qoid.md](qoid.md)。
3. **RP2040 还没上过真机**（能编能链）；`PIO_USE_DMA` 在 125 MHz 下的分频未验证。

## 正确性与健壮性

### 1. JPEGDEC 非法坐标的加固（小改动，建议做）
主机若给 `x != 0` 的矩形发 JPEG，`draw_mcus` 里 `pDraw->iWidthUsed` 会变成负值，`xe` 被算成
子图内坐标（实测 `xs=208 → xe=63`）、`len` 变成巨大的无符号数，一次这样的 flush 就把
`decoder_task` 卡死在 `tft_video_flush`（真机实测 `submitted/drawn = 2/0`，
`s_ep1_pending_size` 一直挂着，复位才恢复）。
**做法**：`iWidthUsed <= 0` 时直接跳过 flush（这帧不画）。**注意**：这只是防呆，**修不好**
JPEG 局刷 —— 要正确的 JPEG 局刷用 tjpgd，要快一律 QOI。见
[decoder-architecture.md](decoder-architecture.md)。

### 8. "小矩形连发打挂 USB"：**未复现，已否定**；另有一次未定因的 HardFault
旧说法已删，`--gap-ms` 默认值也改回 0。三次"复现"逐条核对后：前两次看到的是**整块宿主机
失联 1~2 分钟后自己重启**（内核日志没留下、没有定因）；后来能查到的三次重启都是人工
`systemd-logind: The system will reboot now!`；第三次挂着 gdb、在每个 `ft6236_*` 调用上断点，
每秒把 CPU 停 90 次。**无调试器复测**（gdb 全程 detached）：`tightloop.py 3000/6000 32 0`
（2300~2450 rect/s，errors=0）、`desktop_codecs.py --device --codec qoi --frames 150/300
--gap-ms 0` 全部 rc=0；跑完 `CFSR = 0`、`HFSR = 0`、`drawn == submitted = 16650`、宿主机
`uptime` 没归零。**剩下一个真问题**：排查开始时 Pico 停在 HardFault（`CFSR = 0x8200`），
无调试器复现不出来，最可能是调试会话引起（**未验证**）。2026-09-26 第二次现场
`CFSR = 0x00080000`（NOCP，Handler 里升级、帧里没有 FP 上下文），签名不同，仅用 openocd
halt/读/resume 不触发。现场解读见 [debugging.md](debugging.md)。

### 14. EP1 载荷按 64 字节粒度被破坏（**未定位**）
→ 完整记录见 [ep1-payload-corruption.md](ep1-payload-corruption.md)。两行摘要：洪水负载下
偶发收到"丢掉 64 字节整数倍、后面整体前移"的流（丢的是包不是比特），设备不报警；
下一轮第一件事是主机抓 `usbmon` + 固件全量取证。

## 性能与验证

### 2. bootlogo 重构带来的 +5.6%（未结案）
A/B 交替烧写（同一主机同一脚本、`full/solid（单次传输）`，两侧都稳到 ±0.02 ms）：
HEAD（logo 由独立 `bootlogo_task` 画）**5.20 / 5.23 ms**，工作区（logo 画在 `decoder_task` 里）
**5.50 / 5.52 ms**。已用单变量实验排除：解码/刷屏指令序列（归一化反汇编逐条相同）、
`EP1_RD_BUF_SIZE` 64 ↔ 128 KB、`decoder_task` 栈 1024 ↔ 4096 words、`DECODER_STATS`、
核分配。**剩余嫌疑**是重构本身（连同被删掉的 `decoder_mutex`）—— 稳态下唯一留下的差别是每个
`decoder_drawimg` 少了两次 mutex 进出。**处置**：该重构与 RP2040 那批改动互相独立，可以单独
回退换回这 5.6%（影响面只限"整屏单次传输"这一档）。完整记录见 [fps-bench.md](fps-bench.md)。

### 9. 量一下 JPEG 路径到底卡在设备还是链路
JPEGDEC 载荷最小（全屏 JPEG 约 20~40 KB，链路只要 ~30 ms），但解码重，很可能是**设备受限**
的路径 —— 也就是"热点函数放 SRAM"真正能收益的场景（QOI 那边实测设备侧只快 6~9%、端到端
看不出来）。要做的：`DECODER_TYPE=1` + `DECODER_STATS=1` 构建，用
`Display.send_raw(jpeg_bytes, 0, 0, 479, 319)` 发全屏 JPEG（仓库里没有发 JPEG 的脚本，得自己
拼），取 `draw_us` 与端到端，再和 XIP / 解码循环进 SRAM 两组对比。**未做**。

### 10. RLE 也换成"非回调 + band 乒乓"（**未做**）
`rgb565_rle_decompress()`（非回调版，缓冲由调用方给）和 QOI 那边一样存在。QOI 换成"非回调 +
band 乒乓"后设备侧快了 22~48%（见 [qoi.md](qoi.md)），而 RLE 的回调版同样是每像素一套
"累计 + 容量检查 + 可能回调"的宏，**预期收益相当，但没测**。做法照抄 `qoi_drawimg`：
按 `rect_px <= LZ4_BAND_PIXELS` 分支，用非回调版解到一个 band 缓冲、两块乒乓、帧末尾不 wait。

### 12. 用修正后的量具重测（2026-09-30 发现模型列被当实测引用）
`desktop_codecs.py` 排序表括号里的时间是模型（`LINK_BYTES_PER_S` 常数），把它和
`device_table` 的实测混用会让结论失真（测量纪律）。已修好脚本（`--device` 时自动标定 +
表头写明来源）。**已复量**：QOI vs LZ4 vs RLE 的桌面区域对比（真实内容上 QOI 比 LZ4 快
27~35%；合成桌面上 LZ4 6/8 区域更小 ⇒ 选型依赖内容，见 [codec-selection.md](codec-selection.md)）。
**还没复量**：RP2040 侧那一组（合宙板接回来用 `desktop_codecs.py --device` 重跑，脚本会自己
标定）；更深的流水线（4 帧槽链接得过但没栈，要拿到它预测的 −23% 得先把每次传输上限调小）。

### 13. 跨帧字典 `DECODER_TYPE 6` 原型
机制、全部轮次与未决项都在 [qoid.md](qoid.md)。当前状态摘要：
- 固件侧原型已落地（`src/decoders/tinyd/` + `decoder.c` 的 `DECODER_USE_QOID`），两块板都编得过；
- DELTA 已不依赖流水线打满（设备按载荷报的序号找窗口，主机取最近一条同矩形条带）；
  慢主机 `qoid_mismatch 0`、整屏 `hist_bytes = 318012`；
- **垂直增量收益不成立**（载荷只降 3.0%，帧时间没改善）；
- `PUD_CMD_GET_QOID`（0x05）设备侧**已应答**（60 B，含 `window`），但驱动仓
  `notes/usb-protocol.md` **尚未同步**这条命令与 `decoder_type=6`；
- 照片/噪声内容超半窗会被拒（面板那块不更新），改成报错还是自动回退是产品决定；
- **未解决**：持续负载下的静默冻结（见 TL;DR 第 2 条）；
- **已办**：默认 `DECODER_TYPE=3` 的局刷回归（5 种窗口 `submitted 300 == drawn 300`、
  `dropped 0`、`oversize 0`、0.88~0.97 MB/s）。

## 平台与可观测性

### 3. RP2040 真机 bring-up
**已验证**：`PICO_BOARD=pico` 能编译能链接，当时 `.data/.bss` 124136 B = 256 KB 的 **47.35%**
（tjpgd 构型 48.11%）；用的是真正的 RP2040 SMP 端口（`portable/ThirdParty/GCC/RP2040/`）；
时钟按板取 **125 MHz**（RP2350 是 150）；ISR 栈落在 SCRATCH_X/Y。
**没验**：上板跑。至少要过 PIO 8080 总线时序、USB 枚举、面板与触摸的引脚配置、
实际解码性能（M0+ 比 M33 慢，"QOI 全屏 5 ms"这个量级大概率不成立）、
`PIO_USE_DMA` 在 125 MHz 下的分频是否正确。

### 4. 堆失败可见性（与 heap 选型无关）
`configUSE_MALLOC_FAILED_HOOK 0` 且没有实现 `vApplicationMallocFailedHook()`；`xTaskCreate`
的返回值全都没检查。堆耗尽时的表现是**静默少一个任务**。`heap_3.c` 的 `pvPortMalloc` 里本来
就有调用 hook 的分支，开配置 + 实现钩子即可（打印并停下）。见 [pitfalls-memory.md](pitfalls-memory.md)。

### 5. RP2040 的栈溢出保护
RP2040（Cortex-M0+）**没有 PSPLIM**，任务栈溢出是静默踩内存（RP2350 上会 fault）。
建议在那边的 port 上开 `configCHECK_FOR_STACK_OVERFLOW 2`（现在两边都是 0），
或至少给关键任务留足余量。各任务实测峰值见 [debugging.md](debugging.md)。

### 6. 可选：ISR 栈保护
中断栈每核 2 KB，**没有 MSPLIM**，也没开 `PICO_USE_STACK_GUARDS`，溢出不会立刻 fault
（"不要把解码放进 USB 中断"这条规矩的根源）。`PICO_USE_STACK_GUARDS=1` 是可选做法，
**未验证**。

## 用户空间 / 驱动侧

### 7. 把 LZ4 接到驱动里（用户层已经验证完）
设备侧的 LZ4 解码已重新设计并验证（见 [lz4.md](lz4.md)）：每个传输一个 band，主机按
`band_pixels` 分带即可。驱动侧要做的是：用**内核内置**的 `LZ4_compress_default()`（`lib/lz4`，
`EXPORT_SYMBOL`）编码，**不要**把 QOI/RLE 源文件 vendor 进内核；按 `pud->max_band_pixels`
（`PUD_CMD_GET_CAPS` 上报）分带；注意 band 的**原始**大小（像素数 × 2）也要 ≤ 43674 B。
驱动现在的 `rgb565_qoi.c` 就是"多余源文件"的例子，接完 LZ4 可以删掉。
用户层怎么测的：`tools/img_viewer.py --codec lz4`、`tools/codec_compare.py --codec lz4`。

## 零碎

- TFT 层那把 `#if TFT_BUS_TYPE`（8 处）重构成总线虚表：**有意不做** —— 这个库被 20 来个板子
  配置共用，我们只能 build 验证自己这块，纯好看、纯风险。见 [pitfalls-display.md](pitfalls-display.md)。
- `include/pud.h` 的 `struct decoder_data { u8 type; }` 疑似孤儿（只有定义），**未核实**。
- `notes/` 里引用 `CMakeLists.txt` 时**别写行号** —— 已经漂过一次。
- `PUD_MAX_TRANSFER` 别随手动：32 KB 省 109 KB RAM，但小载荷（纯色）会从 4.6 涨到 6.5 ms/帧
  （band 8→15），128 KB 之前量过是持平的；RP2350 保持 64 KB、RP2040 保持 32 KB
  （见 [codec-selection.md](codec-selection.md)）。
- 提交与推送是两件事：默认**只提交、不推送**，推送需要人工放行（见仓库根 `AGENTS.md` 铁律）。
  四个仓库都有没推的本地提交（固件仓最多，20+ 个）；要推送时逐个 `git log --oneline @{u}..`
  确认，别照抄笔记里的数字（会漂）。

## 相关

- 解码器家族索引：[decoders.md](decoders.md)
- 危险项细节：[qoid.md](qoid.md)、[ep1-payload-corruption.md](ep1-payload-corruption.md)
