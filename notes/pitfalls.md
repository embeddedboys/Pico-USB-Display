# 踩坑合集

按主题分组。每条都是**实际发生过**的问题。

---

## 一、并发与上下文

### 1.1 在 USB 中断里解码 → HardFault

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

`decoder_task` 的栈给到 **4096 words**（其它任务只有 256）。

**排查经验**：`CFSR` 里的 `STKERR`/`MSTKERR` 基本可直接判定"某处栈不够"，
优先怀疑在中断或小栈上下文里干了重活（解码、大数组、`printf`）。

### 1.2 帧槽满时丢帧 → 局部刷新残影

**现象**：局部刷新很快，但拖动窗口/动画后屏幕上留下残影（旧内容残留）。

**根因**：只有 2 个帧槽。主机以数百帧/秒推送局部刷新时解码跟不上，
`decoder_submit_frame()` 在无空闲槽时**静默丢弃整帧**。丢掉的那帧含某区域的最新内容，
之后又被一帧**旧内容**覆盖回去 → 该区域一直保持错误的旧像素。

**修法**：**EP1 流控（背压）** —— 槽满时不武装 EP1，让主机的 bulk 传输自然阻塞。
详见 [decoders.md](decoders.md) 与 [usb-protocol.md](usb-protocol.md)。

修好后实测 `g_decoder_stat_dropped` 恒为 0，`drawn` 恰好落后 `submitted` 1 帧。

### 1.3 超大帧会卡死固件

单帧压缩结果超过 `DECODER_FRAME_MAX`（64 KB）时，`decoder_submit_frame()` 会截断，
截断的流解码失败；更糟的情况是超过 `EP1_RD_BUF_SIZE` 造成状态错乱，固件看起来"死了"。

卡死后用 gdb 复位即可恢复：

```bash
gdb-multiarch -q -nh -ex "target extended-remote localhost:3333" \
  -ex "monitor reset run" -ex "detach" -ex "quit"
```

**预防**：主机侧按像素分带（每带 ≤ 21839 像素，最坏 3 字节/像素），
保证单帧永不超限。见 `PUD-kernel-drivers/notes/display-and-refresh.md`。

---

## 二、显示与外设

### 2.1 PIO + DMA 刷屏在负载下卡死

**现象**：负载上来后显示冻结，gdb 看到卡在 `dma_channel_is_busy()` 自旋循环里
（`i80_write_buf_rs`）。

**观察**：DMA 的 `TREQ_SEL` 读出来是 `0x20`，而期望值是 `1` —— PIO 与 DMA 的
DREQ 握手没有正确建立。

**修法**：`CMakeLists.txt` 里关掉 DMA，走 PIO 轮询路径：

```cmake
set(PIO_USE_DMA 0)
```

代价是 CPU 占用升高，但对这个应用可接受。**想改回 DMA 必须先做压力测试**，
并确认 `TREQ_SEL` 取值与 PIO 的 DREQ 编号匹配。

### 2.2 JPEGDEC 的子图裁切

`draw_mcus` 里必须用 `pDraw->iWidthUsed`，**不是** `pDraw->iWidth`：

```c
pDraw->iWidth       /* 错误：整图宽度 */
pDraw->iWidthUsed   /* 正确：本次实际解码的宽度 */
```

用错会让每行像素偏移错位。这是最终放弃 JPEGDEC 转向 QOI 的原因之一 ——
QOI 解码器按像素处理，任意子矩形都正确，没有 MCU 对齐/裁切问题。

---

## 三、内存

### 3.1 静态 RAM 占用：两个大块吃掉了 256 KB

用 `arm-none-eabi-size` / `nm --size-sort` 实测（当前 QOI 固件）：

| 符号 / 段 | 大小 | 说明 |
| --- | --- | --- |
| `ep1_read_buffer` | **131072 B**（128 KiB） | `EP1_RD_BUF_SIZE`，在 `.noncacheable` |
| `s_frames` | **131104 B**（128 KiB） | `DECODER_FRAME_SLOTS(2) × DECODER_FRAME_MAX(65536)` + 32 B 头，在 `.bss` |
| `qoi_buf_a` + `qoi_buf_b` | 15360 B（15 KiB） | `480 × QOI_BUF_ROWS(8) × 2 B` × 2，在 `.bss` |
| `.data` | 3648 B | 已初始化变量 |
| `.noncacheable` | 132348 B | USB 缓冲单独放在不 cache 的区 |
| `.bss` | 149804 B | 其余静态变量 |
| **data + bss 合计** | **284472 B（≈278 KiB）** | 约占 512 KB 主 SRAM 的 54% |

`size` 输出（当前固件）：

```
   text    data     bss     dec     hex  filename
  92324  132348  152124  376796  5bfdc  pico-usb-display.elf
```

链接脚本可用的主 SRAM 区间是 `0x20000000`–`0x20080000`（512 KB）。
当前 `.bss` 顶端（`end`）在 `0x20045d7c`。

**结论**：EP1 缓冲（128 KB）+ 帧槽（128 KB）= 256 KB，已经吃掉一半 SRAM。
所以**不要轻易把 `DECODER_FRAME_MAX` 或 `DECODER_FRAME_SLOTS` 翻倍** ——
改成 2 × 128 KB 就会溢出（曾评估过：约 540 KB > 512 KB）。
若确实需要更大的单帧，正确方向是：
- 保留流控（丢帧问题已经解决，"加大缓冲"不再必要）
- 或者把帧槽改成"指针 + 两级缓冲"以复用 EP1 的 128 KB

### 3.2 `configTOTAL_HEAP_SIZE` 在本项目里**不起作用**（重要）

`FreeRTOSConfig.h` 定义了：

```c
#define configTOTAL_HEAP_SIZE           (128*1024)
#define configAPPLICATION_ALLOCATED_HEAP 0
```

看起来像是"128 KB 固定堆"，但**实际链接的是 `heap_3.c`**，它的实现是直接包装
编译器的 `malloc`/`free`：

```c
/* heap_3.c */
pvReturn = malloc( xWantedSize );
```

`heap_3.c` **完全不使用 `configTOTAL_HEAP_SIZE`**（也不存在 `ucHeap` 这个符号）。
所以：

- 任务栈、TCB、队列等都由 **newlib `malloc`** 提供；
- 堆通过 `sbrk` 从 `.bss` 末尾向上增长，上限是链接脚本的 `__HeapLimit = 0x20080000`；
- 当前可用堆 ≈ `0x20080000 - 0x20045d7c` ≈ **233 KB**；
- 改 `configTOTAL_HEAP_SIZE` **不会有任何效果** —— 要限制堆得改链接脚本或换 heap_4.c。

想确认实际用的是哪个 heap，看链接了哪个文件即可：

```bash
grep -oE "[^ ]*heap_[0-9]\.c" build-pico2/build.ninja | sort -u
```

栈深度参数（`xTaskCreate`）以 **word** 为单位，RP2350 上 1 word = 4 B：

| 任务 | 栈参数 | 实际字节 |
| --- | --- | --- |
| `usb_task` | 256 | 1 KB |
| `bootlogo_task` | 256 | 1 KB |
| `indev_read` | 256 | 1 KB |
| `decoder_task` | **4096** | **16 KB** |

`xTaskCreate` 失败时返回 `errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY`，
本项目未检查该返回值 —— 堆被耗尽时会静默少一个任务。加任务/加大栈前先算总量。

---

## 四、工程与工具链

### 4.1 默认按 RP2040 配置（最容易踩）

项目默认 `PICO_BOARD=pico`（RP2040）。**直接 `mkdir build && cmake ..` 编出来的固件
在 Pico 2 上跑不起来。**

```bash
# build/         → PICO_BOARD=pico   / PICO_PLATFORM=rp2040     ❌
# build-pico2/   → PICO_BOARD=pico2  / PICO_PLATFORM=rp2350-arm-s ✅
cd build-pico2 && cmake .. -DPICO_BOARD=pico2 && cmake --build . -j8
```

### 4.2 `decoder_names[]` 必须包含所有类型

```c
static char *decoder_names[] = { "tjpgd", "JPEGDEC", "LZ4", "QOI" };
```

这个数组曾漏掉 `"QOI"`，而 `DECODER_TYPE=3` 会越界读。加解码器时同步这里。

### 4.3 `include/bootlogo.h` 是个超大的条件编译文件

按 `DECODER_TYPE` 分支内嵌了多份 logo 压缩数据，4500+ 行。
**不要用 `sed -i` 之类的命令式批处理去改它** —— 曾因参数列表过长导致文件被清空，
最后靠 `git checkout -- include/bootlogo.h` 才恢复。用编辑器的精确替换，或脚本内用 Python。

### 4.4 WSL 里看不到调试器和 USB 设备

WSL 没有 `/dev/bus/usb`，也无法 `mknod` 造出来。所以：

- OpenOCD 必须在 **Windows 宿主机**跑，WSL 通过 `localhost:3333` 连
- 主机侧（pyusb 之类）直接访问 Pico 在 WSL 里是做不到的
- WSL 的 `/tmp` **每次命令调用是独立的**，别把中间产物放那儿再跨调用读

### 4.5 子模块拉取

`lib/` 下是嵌套子模块（`CherryUSB`、`lz4`、`pico-display-lib`，以及 `FreeRTOS-Kernel`
和它的两个 ports 仓库）。必须 `--recursive`，漏了会在编译时报缺文件。

直连 GitHub 失败时，走代理并强制 HTTP/1.1 是验证过可行的组合：

```bash
export https_proxy=http://<proxy>:<port>
git -c http.version=HTTP/1.1 submodule update --init --recursive
```

`ghproxy`、`gitee` 镜像在本项目上验证**不可用**。

### 4.6 修改 `DECODER_TYPE` 后要重新 cmake

它是通过 `target_compile_definitions` 传进去的，只改 `.cmake` 不重新配置不会生效。
另外 `include/bootlogo.h` 的 logo 数据也随类型切换 —— 换了类型别忘了确认开机 logo 正常。
