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

## 4. 堆失败可见性（与 heap 选型无关）

`configUSE_MALLOC_FAILED_HOOK 0` 且没有实现 `vApplicationMallocFailedHook()`；
`xTaskCreate` 的返回值全都没检查。堆耗尽时的表现是**静默少一个任务**。
`heap_3.c` 的 `pvPortMalloc` 里本来就有调用 hook 的分支，开配置 + 实现钩子即可（打印并停下）。

## 5. RP2040 的栈溢出保护

RP2040（Cortex-M0+）**没有 PSPLIM**，任务栈溢出是静默踩内存（RP2350 上会 fault）。
建议在那边的 port 上开 `configCHECK_FOR_STACK_OVERFLOW 2`（现在两边都是 0），
或至少给关键任务留足余量。各任务实测峰值见 [debugging.md](debugging.md)。

## 6. 可选：ISR 栈保护

中断栈每核 2 KB，**没有 MSPLIM**，也没开 `PICO_USE_STACK_GUARDS`，溢出不会立刻 fault
（"不要把解码放进 USB 中断"这条规矩的根源）。`PICO_USE_STACK_GUARDS=1` 是可选做法，
**未验证**。

## 7. 把 LZ4 接到驱动里（用户层已经验证完）

设备侧的 LZ4 解码已经重新设计并验证（见 [decoders.md](decoders.md)）：每个传输一个 band，
主机按 `band_pixels` 分带即可。驱动侧要做的是：

- 用**内核内置**的 `LZ4_compress_default()`（`lib/lz4`，`EXPORT_SYMBOL`）编码，
  **不要**把 QOI/RLE 的源文件 vendor 进内核；
- 按 `pud->max_band_pixels`（`PUD_CMD_GET_CAPS` 上报）分带 —— 和 QOI 用同一套分带逻辑，
  因为 LZ4 的 band 上限就是同一个数（`(65535-16)/3` 像素），所以驱动不需要新字段；
- 注意 band 的**原始**大小（`像素数 × 2`）也要 ≤ 那 43680 B，本设计里两者是同一个限制。

驱动现在的 `rgb565_qoi.c` 就是"多余源文件"的例子，接完 LZ4 可以删掉。
用户层怎么测的：`scripts/img_viewer.py --codec lz4`、`scripts/codec_compare.py --codec lz4`。

## 8. "小矩形连发打挂 USB"：**未复现，已否定**；另有一次未定因的 HardFault

旧笔记说"无间隔连发小矩形会把板子的 USB 打挂"，**这个说法没有站得住脚的证据**，已删；
`--gap-ms` 的默认值也改回 0。原来它基于三次"复现"，逐条核对后是：

- 前两次（`desktop_codecs.py --frames 150` 无间隔、`PUD_DECODER_PINGPONG` 紧循环 A/B）
  看到的是**整块宿主机失联 1~2 分钟后自己重启**（`uptime` 归零）。这两次的内核日志没留下
  （journald 不持久化内核日志），**没有定因** —— "是 USB 触发的"当时只是猜测；
- 后来能查到的三次重启在宿主机日志里都是 `systemd-logind: The system will reboot now!`，
  即**人工下的重启命令**，与 USB 无关；
- 第三次（`touch_draw.py --mode grid`）当时**挂着 gdb**、在每次 `ft6236_*` 调用上断点返回，
  等于每秒把 CPU 停 90 次 —— 那次本来就标了"归因存疑"。

**无调试器复测**（gdb 全程 detached，`DECODER_TYPE=3`，脚本跑在宿主机上）：

| 负载 | 结果 |
| --- | --- |
| `tightloop.py 3000 32 0`（3000 × 32×32，无间隔） | 1.26 s、2373 rect/s、errors=0 |
| `tightloop.py 6000 32 0`（翻倍） | 2.46 s、2443 rect/s、errors=0 |
| `desktop_codecs.py --device --codec qoi --frames 150 --gap-ms 0` | 8 个区域全跑完，rc=0 |
| 同上，`--frames 300 --gap-ms 0` | 同上，各区域中位数与 150 帧那轮一致 |

跑完再读设备：`CFSR = 0`、`HFSR = 0`（这两个故障寄存器是**粘滞**的，复位后一直为 0，
就说明这几组负载没触发任何故障）、`drawn == submitted = 16650`（无丢帧）、宿主机 `uptime`
没归零。所以"连发打挂"不成立，限速不是必要的规避手段。
（表里的 rect/s 是**宿主机**受限的数字，别当设备性能看 —— 见
[architecture.md](architecture.md) 的 225 MHz 一节。）

**剩下一个真问题**：这次排查开始时 Pico 确实停在 HardFault 里（`CFSR = 0x8200`）。
现场寄存器与解读见 [debugging.md](debugging.md) 的"HardFault 定位"。它**无调试器复现不出来**
（上表四组负载跑完 `CFSR`/`HFSR` 都是 0），因此最可能是**调试会话**（在 USB 传输中途停机、
或 gdb 写内存）引起的 —— **未验证**。上桌面重负载前值得再确认一次：跑完读 `HFSR`/`CFSR`，
非 0 就按 [debugging.md](debugging.md) 的步骤抓 PSP 帧。

## 9. 量一下 JPEG 路径到底卡在设备还是链路

JPEGDEC 的载荷最小（一张全屏 JPEG 大概 20~40 KB，链路只要 ~30 ms），但解码重，所以它很可能是
**设备受限**的路径 —— 也就是"热点函数放 SRAM"真正能收益的场景（QOI 那边实测设备侧只快 6~9%、
端到端看不出来，见 [architecture.md](architecture.md)）。

要做的：`DECODER_TYPE=1` + `DECODER_STATS=1` 构建，用 `Display.send_raw(jpeg_bytes, 0, 0, 479, 319)`
发全屏 JPEG（仓库里没有发 JPEG 的脚本，得自己拼），取 `draw_us` 与端到端，再和 XIP / 解码循环
进 SRAM 两组对比。**未做**。

## 10. RLE 也换成"非回调 + band 乒乓"（**未做**）

`rgb565_rle_decompress()`（非回调版，缓冲由调用方给）和 QOI 那边一样存在。QOI 换成
"非回调 + band 乒乓"后设备侧快了 22~48%（见 [decoders.md](decoders.md)），而 RLE 的回调版
同样是每像素一套"累计 + 容量检查 + 可能回调"的宏，**预期收益相当，但没测**。
做法照抄 `qoi_drawimg`：`rle_drawimg` 里按 `rect_px <= LZ4_BAND_PIXELS` 分支，用非回调版解到
一个 band 缓冲，两块乒乓、帧末尾不 wait（下一次 flush 的窗口命令会等前一个传完）。

## 11. 零碎

- TFT 层那把 `#if TFT_BUS_TYPE`（8 处）重构成总线虚表：**有意不做** —— 这个库被 20 来个板子
  配置共用，我们只能 build 验证自己这块，纯好看、纯风险。现状与理由见
  [pitfalls.md](pitfalls.md) 的 2.4。
- `include/pud.h` 的 `struct decoder_data { u8 type; }` 疑似孤儿（只有定义），**未核实**。
- `notes/` 里引用 `CMakeLists.txt` 时**别写行号** —— 已经漂过一次（加 tjpgd 链接行之后）。
- `PUD_MAX_TRANSFER` 别随手动：32 KB 省 109 KB RAM，但小载荷（纯色）会从 4.6 涨到
  6.5 ms/帧（band 8→15），128 KB 之前量过是持平的；RP2350 保持 64 KB、RP2040 保持 32 KB
  （实测见 [decoders.md](decoders.md) 的 "USB 传输缓冲不背锅" 一节）。
- 提交与推送是两件事：默认**只提交、不推送**，推送需要人工放行（见仓库根 `AGENTS.md` 铁律 1）。
  四个仓库都有没推的本地提交（固件仓最多，20+ 个）；要推送时逐个
  `git log --oneline @{u}..` 确认，别照抄笔记里的数字（会漂）。
