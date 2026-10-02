# FPS 基准与真机验收

> `tools/fps_bench.py` 量的是"链路 + 设备"的端到端稳态；真机验收（EP1 自愈、复位后枚举）
> 都靠设备侧计数器与主机侧错误码对齐来判断。

## TL;DR

- `fps_bench.py` 计时段只含 EP0 控制请求 + EP1 批量传输，编码在计时前预生成，所以反映链路 + 设备。
- **`min`** ≈ 纯 USB 传输上限（设备空闲）；`median / min` 大 ⇒ 瓶颈在设备侧，接近 1 ⇒ 瓶颈在 USB 带宽。
- **跨版本比较必须带用例标签**：`(单次传输)` 与分带用例的地板差一倍以上，5.06 ms 只可能出自 `(单次传输)`。
- 判定"改动有没有影响性能"要 A/B 交替烧写、各测多次，不要跨时间比单次数字。
- 未结案：bootlogo 并进 `decoder_task` 后 `full/solid（单次传输）` 慢了 5.6%。

## 全刷 / 局刷 FPS 基准

`tools/fps_bench.py` 要求设备**没有被 `pud` 驱动占用**（pyusb 需要 claim 接口）：

```bash
sudo cp 60-pico-usb-display.rules /etc/udev/rules.d/   # 装一次，之后免 root
sudo rmmod pud
./tools/fps_bench.py --frames 200
./tools/fps_bench.py --dry-run     # 不接设备也能先看各用例载荷大小
```

用例与关注点：

| 用例 | 说明 |
| --- | --- |
| `full / solid,gradient,photo,noise` | 全刷四档内容，QOI 体积从 2.5 KB 到 444 KB |
| `full / <pattern> (单次传输)` | 整帧能装进一次传输时，对比消耗在"驱动分带规则"上的开销 |
| `partial / 64x64, 128x64, 480x8` | 固定居中窗口，内容逐帧变化 |

输出里两个值最关键：

- **`min`** —— 最快的一帧。此时设备空闲（帧槽都是空的），所以它≈**纯 USB 传输**上限。
- **`median`** —— 稳态周期。`median / min` 比值大说明**瓶颈在设备侧**（解码/刷屏）；
  接近 1 说明**瓶颈在 USB 带宽**。

> **跨版本比较必须带用例标签。** 两个 full 用例的地板差一倍以上：`(单次传输)` 整帧一次发完
> （纯色 min ≈ 2.6 ms），而分带用例要 8 段、每段一次往返（纯色 min 就已经 ≈ 5.7 ms，
> `steady/min ≈ 1.04`，判定"USB 受限"）。也就是说 **5.06 ms 这种数字只可能出自 `(单次传输)`**，
> 拿它跟分带的 5.9 ms 比会得出完全错误的结论。历史优化链（36.27 → 8.00 → 5.63 → 5.06 ms）
> 用的都是 `full/solid（单次传输）`。

> 主机的 USB 会话本身也会漂：同一版固件在不同时间测，`min` 与稳态会一起上下浮动 0.1~0.4 ms
> （设备被反复复位/重枚举）。**要判定"改动有没有影响性能"，必须 A/B 交替烧写、各测多次看是否
> 稳定分离**，不要跨时间比单次数字。

### 一个未结案的偏差：bootlogo 并进 `decoder_task` 后 +5.6%

A/B 交替烧写（同一主机、同一脚本、各 2 次，`full/solid（单次传输）`，都稳到 ±0.02 ms）：

| 固件 | 每帧 |
| --- | --- |
| HEAD（logo 由独立 `bootlogo_task` 画） | 5.20 / 5.23 ms |
| 工作区（logo 画在 `decoder_task` 里） | 5.50 / 5.52 ms |

**已用单变量实验排除**（每项都各测 2~3 次）：解码/刷屏指令序列（归一化反汇编后 `qoi_flush`、
`rgb565_qoi_decompress_callback` **逐条相同**）；`EP1_RD_BUF_SIZE` 64 KB ↔ 128 KB（都是
5.50 ms）；`decoder_task` 栈 1024 ↔ 4096 words（都是 5.50 ms，**栈收缩本身性能中性**）；
`DECODER_STATS` 0/1；核分配（两种都在 core 1）。

**剩余嫌疑**：bootlogo 重构本身（连同被删掉的 `decoder_mutex`）—— 稳态下它唯一留下的差别是
每个 `decoder_drawimg` 少了两次 mutex 进出。影响面只限"整屏单次传输"这一档（设备侧受限）；
局刷与 DRM damage 那几档是 USB 受限，不受影响。处置：该重构与 RP2040 那三项改动（协议校验、
按板分缓冲、能力查询）**互相独立**，可以单独回退换回这 5.6%。**未结案，别当成已解决。**

编码器与固件/驱动共用的 `rgb565_qoi.c` **逐字节一致**（已用 solid/gradient/noise/run 四类图案
对照 C 输出验证），所以这里量到的字节数就是驱动实际会发的字节数。

> 注意：测 `480x320` 全刷时驱动规则会切成 **8 段**（`21835 / 480 = 45` 行/段，
> `ceil(320/45) = 8`）。photo/noise 的整帧流超过固件帧槽上限，**必须**分带。

## 复位后的枚举

用 `monitor reset run` 重启固件后，主机会看到一次 USB disconnect + connect，驱动会重新 probe。
主机的 `dmesg` 里能看到 `pud_drm_setup` 一路到 `pud_drm_pipe_enable`。如果列表里旧的 `cardN`
还在、新的多出来一个，那是正常的（旧节点会被 udev 清掉）。

## EP1 自愈的验收（真机）

在**板子上**跑 pyusb 脚本，逐个触发失效，每步之间发一帧正常画面。前提：

- `pud` 驱动 unbind 掉（`echo 6-1:1.0 > /sys/bus/usb/drivers/pud/unbind`，跑完再 bind 回来）；
- 板子上有 pyusb（缺 `ensurepip` 时先 `apt install python3.12-venv`）：
  `python3 -m venv ~/pud-venv && ~/pud-venv/bin/pip install pyusb`；
- 设备侧计数器用 gdb 读 `s_ep1` / `g_ep1_stat`（见 [debugging.md](debugging.md) 的"只读状态"），
  脚本前后各读一次比增量（计数器按 MCU 上电清零）。

| 步骤 | 主机动作 | 期望 |
| --- | --- | --- |
| 基线 | 发一帧 64x64 | 成功 |
| 空闲 | `sleep 5` 后发一帧 | 成功（空闲不需要任何补救，`stale` 不增长） |
| 中途断流 | header 声明 4000 B 只发 100 B，`sleep 1.2` 后发一帧 | 成功，`stale` +1 |
| ZLP | header 声明 52 B + 52 B 载荷（正好 64 B，主机会补一个 ZLP），再发一帧 | 成功（实测 5/5，计数不变） |
| 全屏 | 发一帧 480x320 | 成功 |
| 超长 header（放最后） | header 声明 `> EP1_RD_BUF_SIZE`，再发一帧正常画面 | **主机侧看不到错误**（设备丢弃这一笔并重新武装），`oversize` +1，下一帧照常成功 |

**为什么"超长"不再产生 stall**：设备侧把不可信的 header **丢弃并重新武装**，而不是 stall EP1
（见 [usb-protocol.md](usb-protocol.md) 的"失效与自愈"）。stall 会让主机 `clear_halt()` 重试，
实测那条路会把宿主控制器卡死。

正常主机（驱动刷屏）下应当**永远**是 `bad = 0`、`oversize`/`stale` 不增长、`dropped = 0`：
受控测量 20 s 全屏刷新 = 181 笔传输 / 182 次完成 / **0 失败**。

**读计数时的两个坑**：一是每步用 `unbind` 拆驱动会打断在途传输，一轮验收下来 `oversize` 会涨
十几（实测 15），那是拆装的代价、不是故障 —— 判据是 `bad`/`dropped` 保持 0 且
`submitted == drawn`；二是这些计数器按 MCU 上电清零，所以**只能比前后增量**，不要看绝对值。

## 相关

- 调试器、HardFault、任务栈：[debugging.md](debugging.md)
- 流控与计数器定义：[frame-pipeline.md](frame-pipeline.md)
- 脚本用法：[scripts.md](scripts.md)
