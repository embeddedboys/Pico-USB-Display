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

## 8. 小矩形连发会把 RK3588 的 USB 打挂（未定因，桌面场景要查）

`scripts/desktop_codecs.py --device` 第一次跑用的是 `--frames 150`、**无间隔**连发 6 个
矩形（每次 EP0 窗口 + EP1 bulk，载荷 0.5~31 KB），跑到中途**整块板子失联**：ssh 与 ping
都没响应约 1~2 分钟，随后自己重启（`uptime` 归零）。

**Pico 侧没事**：失联期间用 SWD 连上去看，它在正常跑（PC 在固件里），所以是**主机侧**的
问题。旧启动的内核日志没能留下（journald 没持久化内核日志），**因此没定因** —— 是 dwc3
控制器、libusb 还是 Pico 的某种应答触发，**未验证**。

加 3 ms 间隔后，同样负载跑完三轮（QOI / LZ4 / RLE 各烧一次）都正常，所以脚本默认
`--gap-ms 3`。

**第二次复现**（`PUD_DECODER_PINGPONG` 的紧循环 A/B，`codec_compare.py --frames 60`
无间隔）：跑到 QOI 那一档时板子再次失联并自行重启，另外两档因此没测到数据 ——
所以它与编解码器无关，就是"无间隔 + 小载荷 + 多次传输"这个组合。

**这条对桌面很关键**：桌面的负载恰恰就是**大量小矩形**。如果真的连发就能打挂主机，
那么"一次刷新一个 URB"的设计要在驱动侧确认（合并脏区、限速、或查 dwc3）。复现方式：
`--frames 150 --gap-ms 0`，但**会把板子打挂**，要有人能断电重启再做。

## 9. 把 EP4 触摸接到驱动里（固件/用户层已经做完并验证）

协议见两个仓的 [notes/usb-protocol.md](usb-protocol.md)。驱动侧要改的点（都是旧版
"跑一会儿就死"的原因）：

1. **去掉每个样本一次的 `REQ_EP4_IN` 控制请求**。设备主动推送，主机只需要一开始
   `usb_submit_urb()` 一次，然后在回调里**无论什么 status 都重新提交**（除非正在卸载）。
   旧版 `if (urb->status) return;` 一遇到错误就永久不再提交 —— 输入就此失灵。
2. **`pud_input_cleanup()` 的顺序**：先 `usb_kill_urb()`（并等 work 结束/置位停止标志），
   再 `usb_free_urb()`、`kfree(ep_int_buf)`；现在 work 可能在 URB 释放后又把它提交回去。
3. `BTN_TOUCH/BTN_LEFT` 与 `ABS_X/ABS_Y` 用设备报的坐标即可（已是显示坐标系，
   范围 480×320）；顺手把 `input_set_abs_params` 那两行从 `ABS_MT_*` 改成
   `ABS_X/ABS_Y`（现在设的是 MT 宏，而上报的是普通 ABS_X/Y）。
4. 空闲时设备不发帧 → URB 会长时间挂着，这是正常的，别当超时处理。
5. 丢"松手"的兜底：设备只在状态变化时补一帧，主机可以自己加"多久没收到按下帧就认为
   松开"的超时（**未实现**）。

## 10. 零碎

- `main.c` 的 `frame_counter` 是死代码（无任何引用），可删。
- `include/pud.h` 的 `struct decoder_data { u8 type; }` 疑似孤儿（只有定义），**未核实**。
- `notes/` 里引用 `CMakeLists.txt` 时**别写行号** —— 已经漂过一次（加 tjpgd 链接行之后）。
- 提交与推送是两件事：默认**只提交、不推送**，推送需要人工放行（见仓库根 `AGENTS.md` 铁律 1）。
  目前挂着的本地提交：固件仓 8 个、驱动仓 3 个、`rgb565-rle` 仓 1 个（RLE 的 run 批量填充）。
