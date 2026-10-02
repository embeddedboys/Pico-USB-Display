# 踩坑：并发与上下文

> USB 中断里只做搬运；槽满时靠 EP1 背压而不是丢帧。这两条各有一次真实故障背书。

## TL;DR

- 在 `usbd_vendor_ep1_bulk_out()`（USB 中断）里解码 → `CFSR = 0x8200 (STKERR)`；解码一律交给 `decoder_task`（栈 1024 words / 4 KB，实测峰值 632 B）。
- 帧槽满时静默丢帧 = 局部刷新残影；修法是 EP1 流控，判据是 `g_decoder_stat_dropped == 0`。
- 超大帧会被截断或让 EP1 状态错乱，表现为"固件死了"；预防靠主机按像素分带。

## 1.1 在 USB 中断里解码 → HardFault

**现象**：

```
CFSR = 0x8200   (STKERR + ...)
faulting PC: usbd_ep_start_read+126
r2 = <TFT 数据指针>
```

**根因**：`usbd_vendor_ep1_bulk_out()` 运行在 USB 中断上下文，早期版本直接在里面调用解码。
JPEGDEC 解码吃栈远超中断栈容量 → 压栈失败（`STKERR`）；而且耗时操作阻塞 USB 中断。

**修法**：中断里只做搬运，解码交给独立任务。

```c
void usbd_vendor_ep1_bulk_out(...)
{
    /* Do not decode here: this runs on the USB interrupt stack. */
    decoder_submit_frame(decoder_xs, decoder_ys, decoder_xe, decoder_ye,
                         ep1_read_buffer, nbytes);
}
```

`decoder_task` 的栈现在是 **1024 words（4 KB）**（其它任务 256 words / 1 KB）。实测整条解码
路径峰值 JPEGDEC 632 B、tjpgd 600 B、QOI 496 B，4 KB 有约 6 倍余量；早期版本给的 4096 words
是"怕 JPEGDEC 吃栈"的猜测，实测不成立。细节见 [frame-pipeline.md](frame-pipeline.md)。

**排查经验**：`CFSR` 里的 `STKERR`/`MSTKERR` 基本可直接判定"某处栈不够"，优先怀疑在中断或
小栈上下文里干了重活（解码、大数组、`printf`）。

## 1.2 帧槽满时丢帧 → 局部刷新残影

**现象**：局部刷新很快，但拖动窗口/动画后屏幕上留下残影（旧内容残留）。

**根因**：帧槽有限（早期 2 个，现在 3 个）。主机以数百帧/秒推送局部刷新时解码跟不上，
`decoder_submit_frame()` 在无空闲槽时**静默丢弃整帧**。丢掉的那帧含某区域的最新内容，
之后又被一帧**旧内容**覆盖回去 → 该区域一直保持错误的旧像素。

**修法**：**EP1 流控（背压）** —— 槽满时不武装 EP1，让主机的 bulk 传输自然阻塞。
详见 [frame-pipeline.md](frame-pipeline.md) 与 [usb-protocol.md](usb-protocol.md)。

修好后实测 `g_decoder_stat_dropped` 恒为 0；早期 2 槽时代 `drawn` 恰好落后 `submitted` 1 帧，
现在 3 槽的健康上界是落后 ≤ `DECODER_FRAME_SLOTS - 1` = 2 帧。

## 1.3 超大帧会卡死固件

单帧压缩结果超过 `DECODER_FRAME_MAX`（现在 = `PUD_MAX_TRANSFER`：RP2350 64 KB /
RP2040 32 KB）时，`decoder_submit_frame()` 会截断，截断的流解码失败；更糟的情况是超过
`EP1_RD_BUF_SIZE` 造成状态错乱，固件看起来"死了"。

卡死后用 gdb 复位即可恢复：

```bash
gdb-multiarch -q -nh -ex "target extended-remote localhost:3333" \
  -ex "monitor reset run" -ex "detach" -ex "quit"
```

**预防**：主机侧按像素分带（每带 ≤ 21835 像素，最坏 3 字节/像素 + 12 B EP1 header +
16 B 编解码头尾），保证单帧永不超限。见 `PUD-kernel-drivers/notes/display-and-refresh.md`。

## 相关

- 帧流水线、流控与计数器：[frame-pipeline.md](frame-pipeline.md)
- 显示路径卡死（PIO/DMA）：[pitfalls-display.md](pitfalls-display.md)
