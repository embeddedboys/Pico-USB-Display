# 调试

## 调试器链路

```
Pico (RP2350)  ──CMSIS-DAP──►  Windows 宿主机 (OpenOCD :3333 / telnet :4444)
                                        │
                                   WSL / 开发机 (gdb-multiarch)
```

**OpenOCD 必须跑在能看到 USB 设备的宿主机上**。WSL 里既没有 `/dev/bus/usb`，
也没有权限 `mknod` 造出来，所以调试器只能从 Windows 侧访问，WSL 通过 `localhost:3333` 连。

启动 OpenOCD（Windows）：

```bash
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "adapter speed 10000"
```

- `3333` = gdb 端口
- `4444` = telnet 端口（可直接发 `monitor` 命令）

## 常用操作

### 只读状态（不打断运行）

```bash
gdb-multiarch -q -nh \
  -ex "set pagination off" -ex "set confirm off" \
  -ex "file build-pico2/pico-usb-display.elf" \
  -ex "target extended-remote localhost:3333" \
  -ex "printf \"submitted=%u dropped=%u drawn=%u\n\", \
        g_decoder_stat_submitted, g_decoder_stat_dropped, g_decoder_stat_drawn" \
  -ex "monitor resume" -ex "detach" -ex "quit"
```

**要点**：连接会 halt 目标；读完必须 `monitor resume` 或 `detach` 让它继续跑，
否则屏幕会停住。**千万不要**在只读检查时用 `monitor reset run` —— 那会重启固件、
清掉计数器与显示状态。

### 烧录

见 [build-and-flash.md](build-and-flash.md)。

### 恢复卡死的板子

固件跑飞（HardFault / 卡在 DMA 循环）时，简单复位即可：

```bash
gdb-multiarch -q -nh -ex "target extended-remote localhost:3333" \
  -ex "monitor reset run" -ex "detach" -ex "quit"
```

## 解码流水线诊断

### 计数器

`src/decoders/decoder.c` 里三个 `volatile u32`：

| 变量 | 含义 |
| --- | --- |
| `g_decoder_stat_submitted` | USB 回调交给解码器的帧数 |
| `g_decoder_stat_dropped` | 因无空闲帧槽被**丢弃**的帧数 |
| `g_decoder_stat_drawn` | 已完成绘制的帧数 |

判定标准：

- ✅ `dropped == 0` 且 `drawn` 落后 `submitted` **恰好 1** → 流水线健康（1 帧在途）
- ❌ `dropped` 持续增长 → **流控失效**，症状是局部刷新残影

### 触发负载

- 桌面动画本身就会产生数百帧/秒的局部刷新。
- 想手动加压：向主机的 PUD fbdev 写随机数据。
  **注意 fbN 编号不固定**，先确认哪个是 PUD：
  ```bash
  for f in /sys/class/graphics/fb*; do echo "$f: $(cat $f/name)"; done
  # 找到 pud-drmdrmfb 对应的那个（可能是 fb0 也可能是 fb1）
  ```

### 断点追踪绘制

仓库里有现成的 gdb 脚本（`.pud-test/trace_decode.gdb`、`drawmcu.gdb` 等）用于打印
每次解码的窗口和尺寸：

```gdb
break qoi_drawimg
commands
  silent
  printf "DECODE xs=%d ys=%d xe=%d ye=%d size=%d\n", xs, ys, xe, ye, qoi_size
  continue
end
continue
```

> 断点式追踪会**严重拖慢**固件（每帧都停），只适合确认坐标是否正确，不要用来测吞吐。

## HardFault 定位

从 OpenOCD/gdb 里读故障寄存器：

```gdb
monitor halt
print/x $cfsr      # 需要能访问 SCB，或用 monitor reg
info registers
bt
```

**本项目遇到过的一次真实 HardFault**：

```
CFSR = 0x8200   →  STKERR (bit 9) + INVSTATE... 压栈失败
faulting PC = usbd_ep_start_read+126
r2 = <TFT 数据指针>
```

根因：在 **USB 中断回调里直接解码**，JPEGDEC 吃栈远超中断栈容量 → 压栈失败。
修法：中断里只 `decoder_submit_frame()` 搬运，解码交给 `decoder_task`（栈 4096 words）。

**排查经验**：`CFSR` 的 `STKERR`/`MSTKERR` 基本可以直接判定为"某处栈不够"，
优先怀疑在中断/小栈上下文里做了重活（解码、大数组、printf）。

## 串口日志

调试串口 UART 115200（TX 16 / RX 17），`main.c` 里 `stdio_uart_init_full()`。
开机打印：

```
PICO USB Display
CPU clockspeed: <N> MHz
Decoder type: QOI
calling freertos scheduler, <us>
```

`Decoder type:` 这行会暴露 `decoder_names[]` 的越界问题
（若 `DECODER_TYPE` 超出数组范围，这里会是乱码或崩溃）。

## 全刷 / 局刷 FPS 基准

> 完整的脚本清单、共享模块与依赖选型见 [scripts.md](scripts.md)。

`scripts/fps_bench.py` 是主机端**端到端**基准：计时段只包含 EP0 控制请求 + EP1
批量传输，帧编码在计时前预先生成、不计入，所以数字反映的是链路 + 设备，而不是 Python。

它要求设备**没有被 `pud` 驱动占用**（pyusb 需要 claim 接口）：

```bash
sudo cp 60-pico-usb-display.rules /etc/udev/rules.d/   # 装一次，之后免 root
sudo rmmod pud
./scripts/fps_bench.py --frames 200
```

不带设备也能先看各用例的载荷大小：

```bash
./scripts/fps_bench.py --dry-run
```

用例与关注点：

| 用例 | 说明 |
| --- | --- |
| `full / solid,gradient,photo,noise` | 全刷四档内容，QOI 体积从 2.5 KB 到 444 KB |
| `full / <pattern> (单次传输)` | 整帧能装进一次传输时，对比消耗在"驱动分带规则"上的开销 |
| `partial / 64x64, 128x64, 480x8` | 固定居中窗口，内容逐帧变化 |

输出里两个值最关键：

- **`min`** —— 最快的一帧。此时设备空闲（两个帧槽都是空的），所以它≈**纯 USB 传输**上限。
- **`median`** —— 稳态周期。`median / min` 比值大说明**瓶颈在设备侧**（解码/刷屏）；
  接近 1 说明**瓶颈在 USB 带宽**。

编码器与固件/驱动共用的 `rgb565_qoi.c` **逐字节一致**（已用 solid/gradient/noise/run
四类图案对照 C 输出验证），所以这里量到的字节数就是驱动实际会发的字节数。

> 注意：测 `480x320` 全刷时驱动规则会切成 **8 段**（`21839 / 480 = 45` 行/段，
> `ceil(320/45) = 8`）。photo/noise 的整帧流超过固件帧槽 64 KB，**必须**分带。

## 复位后的枚举

用 `monitor reset run` 重启固件后，主机会看到一次 USB disconnect + connect，
驱动会重新 probe。主机的 `dmesg` 里能看到 `pud_drm_setup` 一路到 `pud_drm_pipe_enable`。
如果列表里旧的 `cardN` 还在、新的多出来一个，那是正常的（旧节点会被 udev 清掉）。
