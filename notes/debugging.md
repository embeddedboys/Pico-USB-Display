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

**复位后主机可能认不回设备**（`lsusb` 里没有 `2e8a:0001`，pyusb 找不到）。现象是
固件其实在正常跑，但 `usb_task` 一直停在"等枚举"上。原因多半是主机没看见一次干净的
断开：**复位前先 halt 住停一会儿**即可（实测 2 秒足够）：

```bash
gdb-multiarch -q -nh -ex "target extended-remote localhost:3333" \
  -ex "monitor reset halt" -ex "shell sleep 2" \
  -ex "monitor reset run" -ex "detach" -ex "quit"
```

> 走断点调试（`monitor reset halt` → 断点 → `continue`）之后再 `reset run`，
> 最容易踩到这个；最稳的是直接重烧一次。

## 解码流水线诊断

### 计数器

`src/decoders/decoder.c` 里三个 `volatile u32`：

| 变量 | 含义 |
| --- | --- |
| `g_decoder_stat_submitted` | USB 回调交给解码器的帧数 |
| `g_decoder_stat_dropped` | 因无空闲帧槽被**丢弃**的帧数 |
| `g_decoder_stat_drawn` | 已完成绘制的帧数 |
| `g_decoder_stat_oversize` | 载荷装不进帧槽的帧数（控制阶段本该先拒掉） |
| `g_decoder_stat_lz4_oversize` | LZ4 专有：band 比 `lz4_band[]` 大（主机分带太粗） |
| `g_decoder_stat_lz4_bad` | LZ4 专有：解码长度 ≠ 窗口像素数（截断/损坏/窗口不符） |

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
修法：中断里只 `decoder_submit_frame()` 搬运，解码交给 `decoder_task`（栈 1024 words / 4 KB）。

**排查经验**：`CFSR` 的 `STKERR`/`MSTKERR` 基本可以直接判定为"某处栈不够"，
优先怀疑在中断/小栈上下文里做了重活（解码、大数组、printf）。

### 判断"到底有没有出过故障"（无调试器排查用）

`HFSR`(0xE000ED2C) / `CFSR`(0xE000ED28) / `BFAR`(0xE000ED38) 都是**粘滞**的：出过故障就一直
置位，直到复位或手工清。所以**复位后跑一段负载，再读这三个寄存器**就能判断这段负载有没有
触发故障，不需要一直挂着调试器（挂调试器本身会干扰 USB 传输）：

```gdb
x/1xw 0xE000ED28     # CFSR
x/1xw 0xE000ED2C     # HFSR
x/1xw 0xE000ED38     # BFAR（CFSR.BFARVALID 有效时才是故障地址）
print/x $pc
```

两个坑：

- `isr_hardfault` 在本工程里是 pico-sdk 的**默认 stub**（`decl_isr_bkpt`），和
  `isr_svcall` / `isr_pendsv` **同一个地址**。所以 `info symbol $pc` / `bt` 会把停在
  HardFault 的核显示成 `isr_svcall` 之类 —— 别被名字骗了，**看寄存器**。
- 出了 HardFault 的核会停在那条 `bkpt` 上，**不会自己恢复**；不用 `monitor reset run`
  复位的话，主机侧看到的就是"设备不响应"。

### 一次未定因的 HardFault（2026-09，怀疑是调试会话引起的）

排查"小矩形连发"时发现的现场（见 [todo.md](todo.md) 第 8 条），无调试器时复现不出来：

```
CFSR  = 0x8200     → PRECISERR + BFARVALID：一次精确的数据访问错，BFAR 有效
HFSR  = 0x40000000 → FORCED：由可配置故障升级上来
BFAR  = 0x130476dc → 出错的数据地址
异常帧 LR = 0xfffffffd → 故障发生在**线程模式、用 PSP**，所以 PSP 上就是故障帧
PSP   = 0x20037c40
  帧内容: R0=0  R1=1  R2=0  R3=0x130476dc  R12=0x12121212
          LR=0x100079ef  PC=0x20011338  xPSR=0x41000000
pxCurrentTCBs[0] = "IDLE0"，pxStack = 0x20037880（256 words）
```

逐条读出来的东西：

- 出错的任务是 **core0 的 idle task**；`PSP − pxStack = 0x3c0`，栈顶在 0x20037c80，也就是说
  只用掉 16 个字 —— **不是栈溢出**。
- `info symbol 0x20011338` → `g_usbd_core+664`（在 `.noncacheable` 里），而 **那个字里存的
  是端点回调 `usbd_vendor_ep2_bulk_in`**。也就是说 PC 落在了一个**函数指针槽的地址**上，
  不是函数本身：控制流被引到了数据区。
- 把 `0x20011338` 处的两个字当代码读：半字 `0x801d` = `strh r5, [r3]`，而 `r3 = BFAR =
  0x130476dc` —— 和"精确存储错误"完全对上。即 CPU 从数据里开始取指，第一条就野写。

**未定因**：无调试器下 3000/6000 × 32×32、150/300 帧桌面负载（都是 `--gap-ms 0`）跑完
`CFSR`/`HFSR` 都是 0，所以最可能是那次排查时**反复停机/写内存的调试会话**造成的。当时用的
`force_touch.gdb` 在断点命令里 `return <常量>` 强改 `ft6236_*` 的返回值（等于替目标改 PC 和栈），
而且有几个 gdb 会话是被 `timeout` 杀掉的（gdb 被杀会把核留在停机状态）—— 这两件事都足以把核
带到不一致的状态，是当前最可疑的来源。**存疑，未验证**；再遇到时按上面的步骤先抓 PSP 帧和
`g_usbd_core` 里那组回调指针，看是哪一个槽被改了。

## 任务栈水位与栈保护

任务栈在建栈时被内核填成 `0xa5`（`tskSET_NEW_STACKS_TO_KNOWN_VALUE`，本工程因为
`configUSE_TRACE_FACILITY 1` 而生效），所以**从 `pxStack` 往上数连续的 `0xa5` 就是没用过的部分**，
峰值 = 栈深 − 这段长度。全程只读内存，不用改固件，也不用调 `uxTaskGetStackHighWaterMark()`。

做法：`monitor halt` 后，用 gdb 的 Python 遍历 FreeRTOS 任务链表
（`pxReadyTasksLists[]`、`pxDelayedTaskList`、`pxOverflowDelayedTaskList`、
`xPendingReadyList`、`xSuspendedTaskList`，取每个 `ListItem_t` 的 `pvOwner`），
再按各 TCB 的 `pxStack` 扫内存。坑：

- `configRECORD_STACK_HIGH_ADDRESS 0` → TCB 里**没有** `pxEndOfStack`，栈深得自己从创建点带进去。
- 实测峰值（当前声明值）：`decoder_task` 496 B / 4 KB —— **空转和满载全屏解码一模一样**，
  整条 QOI 路径（含开机 logo 那一帧）峰值 ≤ 496 B；切到 `DECODER_TYPE=1` 实测
  **JPEGDEC 整帧 480×320 是 632 B**、`DECODER_TYPE=0`（tjpgd）是 **600 B**
  （这两条就是 4096 words 缩到 1024 的依据）；
  `usb_task` 416 B / 1 KB、`indev_read` 432 B / 1 KB、`Tmr Svc` 152 B / 1 KB、
  idle 112~128 B / 1 KB。
- 要量某个 `DECODER_TYPE` 的路径，把 `CMakeLists.txt` 里的 `DECODER_TYPE` 改过去重烧即可 ——
  开机的 logo 正好是按该类型编码的**整屏图**，所以不用主机脚本就能把整条解码路径跑一遍
  （用 `g_bl_priv.bl_lvl == 100` 判断它真的画完了）。
- 想确认某条路径（比如 `printf`）有没有真的进过某个栈：把该栈已用部分的字当返回地址，
  拿 `arm-none-eabi-nm` 的符号表反查。`indev_read` 的栈上能查到
  `_vsnprintf` / `stdio_buffered_printer`，说明 `printf` 的开销已经算在峰值里了。
- 已删除的任务也能量（`bootlogo_task` 曾经就是这种，现在已经并进 `decoder_task`）：
  `heap_3` 下 `free()` 只覆盖块首 8 字节，栈里的 `0xa5` 边界还在。TCB 偏移是
  `pxStack@+52`、`pcTaskName@+64`（`ptype /o TCB_t` 可查），用 RAM 里残留的任务名字符串
  反推出 TCB，再读 `pxStack`。判断任务是否真的没了，还可以看 heap 布局：
  删掉一个任务后，后面任务的 `pxStack` 会整体前移一个 TCB + 栈的距离。

### 栈溢出会不会静默踩内存

**任务栈不会，中断栈会。**

- **任务栈**：端口 `portHAS_STACK_OVERFLOW_CHECKING 1`，建栈时把 `pxStack` 存进上下文的
  PSPLIM 槽，每次切换 `portasm.c` 都 `msr psplim`，越界就是 UsageFault（STKOF）→ HardFault，
  **fail-stop**。验证方法 —— 任意 TCB 的 `pxTopOfStack` 指向的第一个字应等于它的 `pxStack`：
  ```gdb
  print/x *(unsigned int *)pxCurrentTCBs[0]->pxTopOfStack   # == pxCurrentTCBs[0]->pxStack
  ```
  所以 `configCHECK_FOR_STACK_OVERFLOW 0` 在这个端口上可以接受，代价只是溢出表现为卡死，
  没有 `vApplicationStackOverflowHook` 能报告。
- **中断栈（MSP）**：`__StackBottom`~`__StackTop` 每核 **2 KB**（`StackSize = 0x800`，
  core 1 用 `__StackOne*`），下面紧挨着 core 1 的栈 / BSS / 堆顶（`__HeapLimit = 0x20080000`）。
  portasm 里搜不到 `msplim`，CMake 也没开 `PICO_USE_STACK_GUARDS` —— **溢出不会立刻 fault**。
  这就是"解码只能在 `decoder_task` 里做"这条规矩的由来。

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

> **跨版本比较必须带用例标签。** 两个 full 用例的地板差一倍以上：
> `(单次传输)` 整帧一次发完（纯色 min ≈ 2.6 ms），而分带用例要 8 段、每段一次往返
> （纯色 min 就已经 ≈ 5.7 ms，`steady/min ≈ 1.04`，判定"USB 受限"）。
> 也就是说 **5.06 ms 这种数字只可能出自 `(单次传输)`**，拿它跟分带的 5.9 ms 比会得出
> 完全错误的结论。历史优化链（36.27 → 8.00 → 5.63 → 5.06 ms）用的都是
> `full/solid（单次传输）`。

> 主机的 USB 会话本身也会漂：同一版固件在不同时间测，`min` 与稳态会一起上下浮动
> 0.1~0.4 ms（设备今天被反复复位/重枚举）。**要判定"改动有没有影响性能"，必须
> A/B 交替烧写、各测多次看是否稳定分离**，不要跨时间比单次数字。

### 一个未结案的偏差：bootlogo 并进 `decoder_task` 后 +5.6%

A/B 交替烧写（同一主机、同一脚本、各 2 次，`full/solid（单次传输）`，都稳到 ±0.02 ms）：

| 固件 | 每帧 |
| --- | --- |
| HEAD（logo 由独立 `bootlogo_task` 画） | 5.20 / 5.23 ms |
| 工作区（logo 画在 `decoder_task` 里） | 5.50 / 5.52 ms |

**已用单变量实验排除**（每项都各测 2~3 次）：

- 解码/刷屏指令序列 —— 归一化反汇编（去掉地址）后 `qoi_flush`、
  `rgb565_qoi_decompress_callback` **逐条相同**；
- `EP1_RD_BUF_SIZE` 64 KB ↔ 128 KB —— 都是 5.50 ms（所以那次 64 KB 的 .bss 平移不是原因）；
- `decoder_task` 栈 1024 ↔ 4096 words —— 都是 5.50 ms（**栈收缩本身性能中性**）；
- `DECODER_STATS` 0/1、核分配（两种都在 core 1）。

**剩余嫌疑**：bootlogo 重构本身（连同被删掉的 `decoder_mutex`）—— 稳态下它唯一留下的
差别是每个 `decoder_drawimg` 少了两次 mutex 进出。影响面只限"整屏单次传输"这一档
（设备侧受限）；局刷与 DRM damage 那几档是 USB 受限，不受影响。

处置：该重构与 RP2040 那三项改动（协议校验、按板分缓冲、能力查询）**互相独立**，
可以单独回退换回这 5.6%。**未结案，别当成已解决。**


编码器与固件/驱动共用的 `rgb565_qoi.c` **逐字节一致**（已用 solid/gradient/noise/run
四类图案对照 C 输出验证），所以这里量到的字节数就是驱动实际会发的字节数。

> 注意：测 `480x320` 全刷时驱动规则会切成 **8 段**（`21839 / 480 = 45` 行/段，
> `ceil(320/45) = 8`）。photo/noise 的整帧流超过固件帧槽 64 KB，**必须**分带。

## 复位后的枚举

用 `monitor reset run` 重启固件后，主机会看到一次 USB disconnect + connect，
驱动会重新 probe。主机的 `dmesg` 里能看到 `pud_drm_setup` 一路到 `pud_drm_pipe_enable`。
如果列表里旧的 `cardN` 还在、新的多出来一个，那是正常的（旧节点会被 udev 清掉）。
