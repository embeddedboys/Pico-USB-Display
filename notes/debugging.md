# 调试

> 调试器（OpenOCD + gdb）是定位工具：读计数器、抓 HardFault、量任务栈；但**只读检查完必须
> `monitor resume`**，别用 `monitor reset run` 清掉现场。

## TL;DR

- OpenOCD 可跑在 Windows 宿主（WSL 看不到调试器和 USB），也可直接跑在原生 Linux 开发机；gdb 侧命令相同。
- `HFSR`/`CFSR`/`BFAR` 是**粘滞**的：复位后跑一段负载再读，就能判断这段负载有没有触发故障（不需要一直挂调试器）。
- `CFSR` 的 `STKERR`/`MSTKERR` ≈ "某处栈不够"；任务栈有 PSPLIM（越界 fault），**中断栈没有**。
- 任务栈建栈时填 `0xa5`，从 `pxStack` 往上的连续 `0xa5` 就是没用过的部分，峰值 = 栈深 − 该长度，全程只读内存。

## 调试器链路

```
Pico (RP2350)  ──CMSIS-DAP──►  宿主机 (OpenOCD :3333 / telnet :4444)
                                       │
                                  WSL / 开发机 (gdb-multiarch)
```

**OpenOCD 必须跑在能看到 USB 设备的宿主机上**。WSL 里既没有 `/dev/bus/usb`，也没有权限
`mknod` 造出来。启动 OpenOCD（Windows）：

```bash
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "adapter speed 10000"
```

**原生 Linux 开发机不需要 Windows/WSL 这一层**（2026-09 实测）：调试器直接挂在开发机上时，
OpenOCD 就跑在本机，`gdb-multiarch` 连 `localhost:3333` 即可（实测 `1a86:7021` XV-Link
CMSIS-DAP **v2**、`/usr/local/bin/openocd` 0.12.0），gdb 侧命令与上面接法完全相同。
注意 openocd 0.12 把两个核当成 SMP 组，见"halt/resume 的坑"。

- `3333` = gdb 端口；`4444` = telnet 端口（可直接发 `monitor` 命令）。

## 板子上的工作方式（省时间，都是踩过的坑）

真机验证每一轮都很贵，按这个来：

1. **一轮只做一件事**：脚本先写好，一次 `scp` 上去跑完 —— 不要在一轮里串多次 ssh、gdb、构建。
   板子一卡，一轮能白等十分钟。
2. **可能挂住的命令一律套 `timeout`**（`lsusb`、`dmesg`、debugfs 读写、`make`）：USB 栈一卡，
   `lsusb` 会永远不返回。**工具调用自己的超时压到 ≤4 分钟**，挂住要立刻暴露。
3. **gdb 读固件是 30~60 s 级**的操作（连调试器 + halt 双核 + 读符号）：一轮最多读一次；
   能用 `dmesg`/`usbmon` 说清就别读。
4. **固件只编译一次**，产物留在板子上复用。
5. 板子重启后**总线与路径会变**（`6-1` → `3-1`、usbmon 的 `6u` → `3u`）：脚本里动态发现，别写死。
6. 下结论前**两侧都要看**：主机 `dmesg`/`usbmon` 与设备侧计数器（gdb）对得上才算数。
7. **一轮里不要既改代码又做真机验证**：先改完、编译过，再上板。

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

**要点**：连接会 halt 目标；读完必须 `monitor resume` 或 `detach` 让它继续跑，否则屏幕会停住。
**千万不要**在只读检查时用 `monitor reset run` —— 那会重启固件、清掉计数器与显示状态。

### 恢复卡死的板子

```bash
gdb-multiarch -q -nh -ex "target extended-remote localhost:3333" \
  -ex "monitor reset run" -ex "detach" -ex "quit"
```

**复位后主机可能认不回设备**（`lsusb` 里没有 `2e8a:0001`，pyusb 找不到）。现象是固件其实在
正常跑，但 `usb_task` 一直停在"等枚举"上。原因多半是主机没看见一次干净的断开：**复位前先
halt 住停一会儿**即可（实测 2 秒足够）：

```bash
gdb-multiarch -q -nh -ex "target extended-remote localhost:3333" \
  -ex "monitor reset halt" -ex "shell sleep 2" \
  -ex "monitor reset run" -ex "detach" -ex "quit"
```

> 走断点调试（`monitor reset halt` → 断点 → `continue`）之后再 `reset run`，最容易踩到这个；
> 最稳的是直接重烧一次。

**另一种样子：`lsusb` 里还在，但每个请求都 `EIO`**（2026-09-30，直插 xHCI 根口）。
`openocd ... program ... reset run` 烧完，`lsusb` 照常列出 `2e8a:0001`，可 pyusb 的 `GET_CAPS`
控制请求报 `[Errno 5] Input/Output Error`，openocd 也打出 `could not read product string ...
Input/Output Error`。设备侧读到的是：`s_configured = 0`、`s_ep1` 全 0、`ADDR_ENDP = 0`（地址被
复位清掉了）、`SIE_CTRL` 上拉开着、两个核都在 idle 任务里、`CFSR`/`HFSR` = 0 —— 固件好好的，
只是**没被重新枚举**：复位太快，主机没看到断开，还拿旧地址跟它说话。上面的"halt 住停 2 秒再
`reset run`"这次**没用**；**从主机侧复位端口**立刻恢复（`dmesg`：`reset full-speed USB device
number 68`，随后 caps 正常）：

```bash
.venv/bin/python -c 'import usb.core; usb.core.find(idVendor=0x2e8a, idProduct=0x0001).reset()'
```

烧完就跑一次这条，再开始测；否则第一步就会看起来像"新固件把 USB 弄坏了"。

### halt/resume 的坑：两个核是一个 SMP 组（2026-09 实测）

openocd 0.12 的 `rp2350.cfg` 把 `rp2350.cm0` / `rp2350.cm1` 当成**一个 SMP 组**：`resume` 会
试着把整组一起放开，只要有一个成员不在停机状态就失败：

```
Error: [rp2350.cm1] not halted
Warn : [rp2350.cm0] resume of a SMP target failed, trying to resume current one
Error: [rp2350.cm0] resume failed
```

**失败的 resume 什么都不会恢复** —— 已经被 `halt` 的那个核就留在停机状态（主机侧表现就是
"设备不响应"，很像固件挂死，但 flash 内容没坏）。正确顺序是**先让整组都停，再整组放开**：

```tcl
targets rp2350.cm0
halt
targets rp2350.cm1
halt
targets rp2350.cm0
resume
```

自查 `targets` 的 State 列：halt 过的目标写 `halted due to debug-request`（停在 pico-sdk 那个
`bkpt` stub 上时是 `halted due to breakpoint`）；**新会话里显示 `unknown` 是"运行中"的正常
显示**，别把它当成挂死。

## 解码流水线诊断

计数器清单、健康判据、加压方法与断点追踪脚本在
[frame-pipeline.md](frame-pipeline.md)（计数器定义）与 [fps-bench.md](fps-bench.md)（FPS 基准）。

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

根因：在 **USB 中断回调里直接解码**，JPEGDEC 吃栈远超中断栈容量 → 压栈失败。修法：中断里只
`decoder_submit_frame()` 搬运，解码交给 `decoder_task`（栈 1024 words / 4 KB）。
**排查经验**：`CFSR` 的 `STKERR`/`MSTKERR` 基本可以直接判定为"某处栈不够"，优先怀疑在中断/
小栈上下文里做了重活（解码、大数组、`printf`）。

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

- `isr_hardfault` 在本工程里是 pico-sdk 的**默认 stub**（`decl_isr_bkpt`），和 `isr_svcall` /
  `isr_pendsv` **同一个地址**。所以 `info symbol $pc` / `bt` 会把停在 HardFault 的核显示成
  `isr_svcall` 之类 —— 别被名字骗了，**看寄存器**。
- 出了 HardFault 的核会停在那条 `bkpt` 上，**不会自己恢复**；不用 `monitor reset run` 复位的
  话，主机侧看到的就是"设备不响应"。

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
- `info symbol 0x20011338` → `g_usbd_core+664`（在 `.noncacheable` 里），而**那个字里存的是
  端点回调 `usbd_vendor_ep2_bulk_in`**。也就是说 PC 落在了一个**函数指针槽的地址**上，不是
  函数本身：控制流被引到了数据区。
- 把 `0x20011338` 处的两个字当代码读：半字 `0x801d` = `strh r5, [r3]`，而 `r3 = BFAR =
  0x130476dc` —— 和"精确存储错误"完全对上。即 CPU 从数据里开始取指，第一条就野写。

**未定因**：无调试器下 3000/6000 × 32×32、150/300 帧桌面负载（都是 `--gap-ms 0`）跑完
`CFSR`/`HFSR` 都是 0，所以最可能是那次排查时**反复停机/写内存的调试会话**造成的。当时用的
`force_touch.gdb` 在断点命令里 `return <常量>` 强改 `ft6236_*` 的返回值（等于替目标改 PC 和
栈），而且有几个 gdb 会话是被 `timeout` 杀掉的（gdb 被杀会把核留在停机状态）—— 这两件事都
足以把核带到不一致的状态，是当前最可疑的来源。**存疑，未验证**；再遇到时按上面的步骤先抓
PSP 帧和 `g_usbd_core` 里那组回调指针，看是哪一个槽被改了。

**第二次现场（2026-09-26，原生 Linux，签名不同）**：只做了一次完全"干净"的 gdb 会话 ——
attach → 读三个计数器 → `monitor resume` → `detach`，没有断点、没有写内存、gdb 正常退出 ——
之后 openocd 日志里 cm0 就停在 `Handler HardFault`：

```
CFSR  = 0x00080000 → UFSR.NOCP：在协处理器/FPU 关闭时执行了协处理器指令
HFSR  = 0x40000000 → FORCED
PC    = 0x1000011c → pico-sdk 默认 HardFault stub（内含 bkpt，所以 openocd 报
                     "halted due to breakpoint"）
LR    = 0xfffffff1 → 从 **Handler 模式**升级上来，且帧里没有 FP 上下文
IPSR  = 3；MMFAR/BFAR = 0x20081f0c（两个 VALID 位都没置 → 值无意义）
```

与上一次（`0x8200` 精确访问错、线程模式、PSP 帧里是"从数据取指"的现场）**不是同一个签名**：
这次是 Handler 里升级的 NOCP，**没有压 FP 上下文**。同一轮里只用 openocd 做过三次
halt/读/resume（不挂 gdb）全都没触发，只有 gdb attach 那次出了 —— 指向**调试器对完整上下文
（含 FP/协处理器状态）的保存与恢复**，而不是固件自己的逻辑；仍**未定因**。

恢复：用上面"恢复卡死的板子"的 `monitor reset halt` → `sleep 2` → `monitor reset run`，
一次成功；主机看到干净的断开/重连（设备号 069 → 070），**flash 内容未变**。

## 任务栈水位与栈保护

任务栈在建栈时被内核填成 `0xa5`（`tskSET_NEW_STACKS_TO_KNOWN_VALUE`，本工程因为
`configUSE_TRACE_FACILITY 1` 而生效），所以**从 `pxStack` 往上数连续的 `0xa5` 就是没用过的
部分**，峰值 = 栈深 − 这段长度。全程只读内存，不用改固件，也不用调 `uxTaskGetStackHighWaterMark()`。

做法：`monitor halt` 后，用 gdb 的 Python 遍历 FreeRTOS 任务链表（`pxReadyTasksLists[]`、
`pxDelayedTaskList`、`pxOverflowDelayedTaskList`、`xPendingReadyList`、`xSuspendedTaskList`，
取每个 `ListItem_t` 的 `pvOwner`），再按各 TCB 的 `pxStack` 扫内存。坑：

- `configRECORD_STACK_HIGH_ADDRESS 0` → TCB 里**没有** `pxEndOfStack`，栈深得自己从创建点带进去。
- 实测峰值（当前声明值）：`decoder_task` 496 B / 4 KB（QOI）—— **空转和满载全屏解码一模一样**，
  整条 QOI 路径（含开机 logo 那一帧）峰值 ≤ 496 B；切到 `DECODER_TYPE=1` 实测
  **JPEGDEC 整帧 480×320 是 632 B**、`DECODER_TYPE=0`（tjpgd）是 **600 B**（这两条就是
  4096 words 缩到 1024 的依据）；`usb_task` 416 B / 1 KB、`indev_read` 432 B / 1 KB、
  `Tmr Svc` 152 B / 1 KB、idle 112~128 B / 1 KB。
- 要量某个 `DECODER_TYPE` 的路径，把构建切过去重烧即可 —— 开机的 logo 正好是按该类型编码的
  **整屏图**，所以不用主机脚本就能把整条解码路径跑一遍（用 `g_bl_priv.bl_lvl == 100` 判断它
  真的画完了）。
- 想确认某条路径（比如 `printf`）有没有真的进过某个栈：把该栈已用部分的字当返回地址，拿
  `arm-none-eabi-nm` 的符号表反查。`indev_read` 的栈上能查到 `_vsnprintf` /
  `stdio_buffered_printer`，说明 `printf` 的开销已经算在峰值里了。
- 已删除的任务也能量（`bootlogo_task` 曾经就是这种，现在已并进 `decoder_task`）：`heap_3` 下
  `free()` 只覆盖块首 8 字节，栈里的 `0xa5` 边界还在。TCB 偏移是 `pxStack@+52`、`pcTaskName@+64`
  （`ptype /o TCB_t` 可查），用 RAM 里残留的任务名字符串反推出 TCB，再读 `pxStack`。

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
- **中断栈（MSP）**：`__StackBottom`~`__StackTop` 每核 **2 KB**（`StackSize = 0x800`，core 1 用
  `__StackOne*`），下面紧挨着 core 1 的栈 / BSS / 堆顶（`__HeapLimit = 0x20080000`）。portasm
  里搜不到 `msplim`，CMake 也没开 `PICO_USE_STACK_GUARDS` —— **溢出不会立刻 fault**。
  这就是"解码只能在 `decoder_task` 里做"这条规矩的由来。

## 串口日志

调试串口 UART 115200（TX 16 / RX 17），`main.c` 里 `stdio_uart_init_full()`。开机打印：

```
PICO USB Display
CPU clockspeed: <N> MHz
Decoder type: QOI
calling freertos scheduler, <us>
```

`Decoder type:` 这行会暴露 `decoder_names[]` 的越界问题（若 `DECODER_TYPE` 超出数组范围，
这里会是乱码或崩溃）。

## 相关

- FPS 基准、bootlogo +5.6% 未结案、复位后枚举、EP1 自愈验收：[fps-bench.md](fps-bench.md)
- 帧槽、流控与计数器定义：[frame-pipeline.md](frame-pipeline.md)
- 烧录与构建：[build-and-flash.md](build-and-flash.md)

