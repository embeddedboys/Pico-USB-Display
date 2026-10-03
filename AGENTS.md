# AGENTS.md

## Skills（本仓遵守）

本仓的一切工作遵循工作区 `../AGENTS.md` 约定的四份 skill。**摘要随仓携带**（离线可读），
完整版在工作区 `skills/`。

| skill | 本仓副本 | 一句话 |
| --- | --- | --- |
| Repository Exploration | [`skills/developer-repository-exprolation/Summary.md`](skills/developer-repository-exprolation/Summary.md) | 先理解再修改；证据优先于直觉 |
| Knowledge | [`skills/developer-knowledge/Summary.md`](skills/developer-knowledge/Summary.md) | 首屏结论、事实分级、信息预算、漂移检查 |
| Testing | [`skills/developer-testing/Summary.md`](skills/developer-testing/Summary.md) | tests/tools 分层、oracle 声明、退出码、N 次测量 |
| Code Quality | [`skills/developer-code-quality/Summary.md`](skills/developer-code-quality/Summary.md) | **能跑 ≠ 完成**；可读性有硬标准 |

### 动手前的四行闸门（**强制**）

改任何代码或配置**之前**先写出这四行 ✓。**第 1 行或第 4 行写不出来就停手** ✗ —— 那是在猜 ✗。

```text
已验证：<确认了什么，凭据是什么：代码/实测/构建日志>
仍未知：<还没确认的；不许用推测填空>
最小改动：<只改一处，为什么是这一处>
生效验证：<如何证明改动真的生效：探针 / grep 生成物 / 构建日志里的编译行>
```

**先确认仪器，再相信读数** ✓ —— 宏没被注入、文件没被编译、配置被 defconfig 覆盖，
这三件事的症状都是"结果莫名其妙" ✗。


> 本仓库是 RP2350（Pico 2）上的 USB 显示固件（FreeRTOS + CherryUSB + PIO 8080 TFT）。
> 通用知识库/测试约定见工作区根 [`../AGENTS.md`](../AGENTS.md)；详细知识见 [`notes/`](notes/README.md)。
> 本文只写"必须遵守的约束"和入口；细节一律在 notes，改动前先读。

## 铁律

1. **未经明确指令，不要 `git commit`，更不要 `git push`。** 改完先报告改了什么、
   验证到什么程度，等指令。（曾经把"告诉你提交者身份"误解成"让你提交"。）
2. **构建必须用 `build-pico2/`**（`PICO_BOARD=pico2` / RP2350）。仓库根的 `build/`
   是 RP2040 配置，烧到 Pico 2 上跑不起来。
3. **仓库内不得出现内网/个人信息**：本机绝对路径、内网 IP、口令、代理地址、板子序列号、内部代号。
4. **不要把解码放进 USB 中断**（会 HardFault，见"架构不变量"1）。
5. **不要去掉 EP1 流控**（局部刷新残影的修法，见"架构不变量"2）。
6. **`src/decoders/{qoi,rle,jpegdec,tjpgd}/` 是 vendored**，必须与上游仓库
   （`rgb565-qoi` / `rgb565-rle`）**逐字节一致**：要改行为先改上游、再把文件整体拷回来
   （`cmp` 验证）；编译期差异走它们的 `RGB565_*_SECTION` 钩子。
7. **`include/bootlogo.h` 是 4500+ 行的生成大数组**：用编辑器的精确替换改，
   **不要用 `sed -i` 之类批处理**（曾因参数列表过长把文件清空，靠 `git checkout` 才恢复）。
8. **协议字段改动要成对改驱动仓**（`REQ_*`、`struct pud_ep1_header`、`struct pud_caps`、
   `struct pud_params`、`struct pud_touch_report`），并同步两侧协议文档（见不变量 6）；
   字段只追加、不重排、不复用已退休的编号。

## 提交与身份

- `user.name` = `Wooden Chair`，`user.email` = `hua.zheng@embeddedboys.com`；
  提交一律 `git commit -s`（仓库既有历史都带 sign-off），信息用内核风格
  `模块: 组件: 简述`，一个逻辑改动一个提交。
- 默认分支 `main`；子模块指针改动要和子模块提交一起考虑。
- **提交与推送是两件事**：默认只提交、不推送，推送需要人工放行。

## 构建 / 烧录 / 验证入口

```bash
./build.sh lunch     # 选板子（pico/pico2）+ 面板配置（configs/）+ 烧录方式，记在 .pud-config
./build.sh           # 配置 + 构建；pico2 -> build-pico2/，pico -> build/
./build.sh flash     # 按选中的方式烧（picotool/openocd/gdb/blackmagic/none）
./build.sh flash -n  # 只打印命令行；各条命令的实测状态见 notes/build-and-flash.md
```

等价的手工命令（`build.sh` 内部就是它）：

```bash
cd build-pico2 && cmake .. -DPICO_BOARD=pico2 && cmake --build . -j8
```

- **面板配置来自仓库自己的 `configs/`**（`-DPUD_CONFIG=<名字>` 传给 CMake），不再从子模块
  include；脚本都在 `scripts/`，入口是 `./build.sh`（`configs`/`config`/`flash`/`clean` 也在里面）。
- 子模块要 `--recursive`（CherryUSB / lz4 / pico-display-lib / FreeRTOS-Kernel 及其 ports，
  以及 **pico-turbo**：时钟/电压/flash 分频与 `boards/*.cmake` 都在它里面）。直连 GitHub
  失败时"走代理 + `git -c http.version=HTTP/1.1`"是验证过可行的组合（`ghproxy`/`gitee` 镜像不可用）。
- 烧录：OpenOCD 可跑在 **Windows 宿主机**（WSL 看不到调试器，也无法 `mknod` 出
  `/dev/bus/usb`），WSL 侧用 `gdb-multiarch -q -nh -ex "target extended-remote localhost:3333"`；
  `-q -nh` 是必需的（否则会读 `~/.gdbinit`，装了 gef 之类会直接报错中断）。
  **本机直连也行**（2026-09-27 实测）：Raspberry Pi Debug Probe（CMSIS-DAP `2e8a:000c`）+
  本机 openocd 0.12.0 → `./build.sh flash -m openocd` 直接 `Verified OK`；`FLASH=gdb` 配本机
  openocd 起的 GDB server 同样能烧（本机没有 `gdb-multiarch` 时用 `/usr/bin/gdb` 也行）。
- **应用态不用按 BOOTSEL**：固件带 picoboot 的 reset 接口（描述符里第 2 个接口，
  `0xFF/0x00/0x01`，见 [notes/usb-protocol.md](notes/usb-protocol.md)），
  `./build.sh flash -m picotool --reboot` 会先请板子自己重启进 BOOTSEL 再烧
  （实测 2026-09-27，不需要调试器）。
- **只读检查固件状态时，读完要 `monitor resume`**；别用 `monitor reset run`
  （会清掉计数器和显示状态）。卡死时才用它。
- **首选验证方式：不加载内核驱动**，用 `tools/` 的 pyusb 脚本直连（`tools/pud_usb.py`
  是共享库，每种编码器只有一份，见 [notes/scripts.md](notes/scripts.md)）。设备必须未被
  `pud` 驱动占用；装仓库根的 `60-pico-usb-display.rules` 可免 root —— **`60-` 不能退回
  `50-`**（会被 `/usr/lib/udev/rules.d/50-udev-default.rules` 覆盖而完全失效）。规则同时
  放行面板（`2e8a:0001`）和 BOOTSEL 里的 bootrom（`0003`/`000f`，外加 SDK `stdio_usb` 的
  `0009`/`000a`）；只放行 `0001` 时 `picotool` 会报 `unable to connect. Maybe try 'sudo'`。
- 板子上的工作方式与烧写/测量纪律见 [notes/debugging.md](notes/debugging.md) 与
  [notes/build-and-flash.md](notes/build-and-flash.md)。

## 架构不变量（动了就坏）

1. **解码只能在 `decoder_task` 里做。** 在 `usbd_vendor_ep1_bulk_out()`
   （USB 中断上下文）里解码会因中断栈不足 HardFault（`CFSR` 的 `STKERR`），
   并且会长时间阻塞 USB 中断。`decoder_task` 栈 **1024 words（4 KB）**：
   整条解码路径的实测峰值是 JPEGDEC 632 B / tjpgd 600 B / QOI 496 B（用 `0xa5` 填充反推，
   见 [notes/debugging.md](notes/debugging.md)），4 KB 已有约 6 倍余量。
   这条是**实测**结论，别再凭"JPEGDEC 吃栈"的猜测往上加 —— 它的上下文在
   `.bss`（`&g_jpegdec`），回调只有标量局部。要加之前先测。
2. **EP1 流控必须保留。** 帧槽全忙时**故意不武装 EP1**，让主机的 bulk 传输
   阻塞等待（`usbd_vendor_ep1_tick()`：只有槽空时才武装；每帧提交完/解码任务
   释放槽后都会调用）。
   判定标准：`g_decoder_stat_dropped == 0`，且 `drawn` 落后 `submitted` 不超过
   `DECODER_FRAME_SLOTS - 1` 帧（2 槽时是 1 帧，现在是 3 槽 ⇒ ≤2 ✓ 实测 dropped == 0 ✓）。
   去掉它 = 槽满静默丢帧 = 局部刷新残影。
3. **RAM 很紧。** RP2350 512 KB SRAM 里 `ep1_read_buffer`（64 KB）+
   `s_frames`（**3 × 64 KB**）已经占掉一大块；**RP2040 只有 256 KB 可用**，
   所以这两个尺寸**都不是写死的，由 `PUD_MAX_TRANSFER` 按板子决定**
   （`src/cherryusb/usbd_vendor.h`，RP2350 64 KB / RP2040 32 KB），
   帧槽在 `decoder.c` 里用同一个宏（`DECODER_FRAME_MAX = PUD_MAX_TRANSFER`）并有
   `_Static_assert` 兜底。
   改它 = 改协议，主机靠 `PUD_CMD_GET_CAPS` 问设备（见"架构不变量"第 6 条）。
   **不要把 `DECODER_FRAME_SLOTS` 或 `PUD_MAX_TRANSFER` 翻倍**
   （RP2040 上实测 128 KB + 2×64 KB 时 .data/.bss 达到 RAM 的 109%，直接链接失败）。
   **加一槽要按实测算账**：2 → 3 是值得的（解码完全藏进链路：整屏 78.0 → 73.7 ms ✓，
   tinfl/libdeflate 都一样 ✓；代价 RP2350 +64 KB、RP2040 +32 KB ✓ 两块都编得过），
   4 槽在 RP2350 上能链接但 `.data+.bss` 到 504 KB、**运行时没有栈**（实测起不来）✗ ——
   要更深的流水线得先把每次传输的上限调小（槽随之变小，带数变多反而更利于重叠）。
4. **`configTOTAL_HEAP_SIZE` 在本项目不起作用** —— 链接的是 `heap_3.c`，它只包装 `malloc`
   （无 `ucHeap` 符号）。上限由链接脚本的 `_sbrk` 强制（卡在 `0x20080000`，越界返回 NULL、
   不踩中断栈），所以**缺的从来不是上限而是可见性**：`configUSE_MALLOC_FAILED_HOOK=1` +
   `configCHECK_FOR_STACK_OVERFLOW=2` + `main.c` 的两个钩子已补上，**代价只有 RAM +8 B**
   （换 heap_4 要静态预留 16 KB，而内核只需约 10.3 KB）。详见
   [pitfalls-memory.md](notes/pitfalls-memory.md) 3.2/3.3。
5. **`decoder_names[]` 必须覆盖所有 `DECODER_TYPE`**（曾漏 `"QOI"` 导致越界读），
   现在是 7 项（含 `"QOI+deflate (tinfl)"` / `"QOI+deflate (libdeflate)"` /
   `"QOI+deflate+dict"`），并有 `_Static_assert` 兜底；**不要把编号重排** ——
   `decoder_type` 会通过 `PUD_CMD_GET_CAPS` 上报给主机。
   两种 JPEG 实现都保留：tjpgd（局刷正确但慢）/ JPEGDEC（快但 `x != 0` 会卡死显示），
   见 [notes/decoder-architecture.md](notes/decoder-architecture.md)。
6. **协议字段改动要成对改驱动**（`REQ_*`、`struct pud_ep1_header`、`struct req_ep2_in`），
   并同步两侧协议文档：本仓的 [`notes/usb-protocol.md`](notes/usb-protocol.md)、
   [`notes/usb-params.md`](notes/usb-params.md)、[`notes/touch-ep4.md`](notes/touch-ep4.md)
   与驱动仓的 `notes/usb-protocol.md`（权威定义）。
7. **异步刷新有缓冲区契约**：`tft_async_video_flush()` 返回时传输仍在进行，
   `vmem` 在 `tft_async_video_wait()`（或下一次 flush，它会先完成上一个）返回前
   **不得复用**。QOI 默认路径靠**两块 band 缓冲乒乓**满足（解码 B 时 A 还在传；
   下一次 flush 的窗口命令会等 A 传完），回落到回调版时靠 `qoi_buf_a/b` 满足；
   LZ4 用的是自己那块 `lz4_band` + 每帧 `tft_async_video_wait()`。
   改动解码器或换成单缓冲时必须重新确认这一点。同一时刻只允许一个传输在途。
8. **`include/bootlogo.h` 是按 `DECODER_TYPE` 分支的 4500+ 行大数组**：
   用编辑器的精确替换改，**不要用 `sed -i` 之类批处理**
   （曾因参数列表过长把文件清空，靠 `git checkout` 才恢复）。
   LZ4 那一支是**band 容器**（`[count][offsets][blocks]`，每 band 一个 block），
   不是单个整帧 block —— 原因见第 9 条；`decoder_draw_bootlogo()` 按 `height / count`
   推 band 高度，所以**重新生成时 band 高度必须整除面板高度**。
9. **LZ4 一个 block 不能分块解码**：每个 match 都指回同一 block 之前解出的输出，所以
   整块必须落进一块连续缓冲，该缓冲同时是字典。因此**设备一次只持有一个 band**
   （`lz4_band[LZ4_BAND_PIXELS]`，`LZ4_BAND_PIXELS = ((PUD_MAX_TRANSFER -
   PUD_EP1_HEADER_SIZE - 16) / 3) + 1` ⇒ RP2350 **21837 px = 43674 B**；
   旧笔记写 21840 px / 43680 B，是旧公式没减 12 B EP1 头，以代码为准），
   **主机必须按 `PUD_CMD_GET_CAPS` 上报的 `band_pixels` 分带**，一个传输一个自包含 block。
   放不下或解码长度与窗口不符就计数丢弃（`g_decoder_stat_lz4_*`），**不要截断**。
   整帧 307200 B 的 block 永远解不了 —— 旧实现每帧 `malloc` 308 KB 就是这么坏的。
10. **触摸坐标只有一处变换**：各触摸驱动（ft6236/gt911/cst816d/tsc2007）只返回**控制器
    原始值**，轴序/反向/偏移/钳位全在 `pico-display-lib` 的 `indev.c` 里按
    `indev_dir_for_rotation(TFT_ROTATION)` 做 —— 触摸和显示共用同一个 `TFT_ROTATION`，
    不要再往驱动里塞 `set_dir()` 常量（改前就是这样，结果旋转不跟着走，而且
    `set_dir()` 调用两次会把轴序转回去）。
    EP4 的上报布局（8 字节，`include/pud.h` 的 `struct pud_touch_report`）是**协议字段**，
    改它要同步驱动仓的 `notes/usb-protocol.md`。
11. **面板寄存器写也不能在 USB 中断里做。** 走总线（SPI/PIO）要等硬件，放在厂商请求回调里
    会长时间占着 USB 中断 —— 运行期参数通道因此把 `rotation` 拆成两半：中断里只记账
    （几何 + `indev_set_dir()`，都是几次赋值），MADCTL 写由 `decoder_task` 在画下一帧之前
    执行（`pud_params_flush_display()`，见 [notes/usb-protocol.md](notes/usb-protocol.md)）。
12. **设备必须能自愈：看门狗监督任务不能去掉，也不能挪进被监视的任务里。**
    `main.c` 的 `watchdog_supervisor_task()` 跑在 `tskIDLE_PRIORITY + 4`（高于 decoder +1 与
    usb +3），所以某个任务卡在自旋里时它仍会被调度；它发现停顿后**停止喂**硬件看门狗，
    由芯片复位。判据是 `submitted − dropped − drawn` 持续超时 —— **`dropped` 不能省**：
    `decoder_submit_frame()` 丢帧时也计 `submitted`，只用 `submitted − drawn` 会在设备空闲
    一秒后误复位一台正常的设备（已用主机状态机模拟验证）。原因写进
    `watchdog_hw->scratch[0..1]`，跨复位存活，下次启动会打印恢复次数。
    详见 [notes/qoid.md](notes/qoid.md) 的"自愈看门狗"一节。

## 当前配置（改前先读 notes）

| 配置 | 值 | 说明 |
| --- | --- | --- |
| `DECODER_TYPE` | `3`（QOI） | 图片/视频脚本按 QOI 发；`0`=tjpgd、`1`=JPEGDEC、`2`=LZ4、`4`=RLE、`5`=QOI+deflate（实验；驱动还不会发。**两块板都装得下** ✓ —— RP2040 上 3 帧槽实测 164 KB/264 KB，提交信息里"只 RP2350"的说法不成立 ✗；限制不在设备而在**等级**：主机侧默认已从 level 1 改成 6（字节 −11.2% → −26.6% ✓）。见 [notes/qoiz.md](notes/qoiz.md)）、`6`=QOI+deflate+跨帧字典（实验；持续负载下曾**静默冻结**，根因经续查指向显示总线的无界 DMA 等待、**待真机验证**，现已由不变量 12 的自愈看门狗兜底，见 [notes/qoid.md](notes/qoid.md)）。**编号是协议字段**（`PUD_CMD_GET_CAPS` 上报），不要重排；现在是 cache 变量，`-DDECODER_TYPE=5` 另开构建目录 |
| `OVERCLOCK_ENABLED` | `1` | 板配置 profile 1：RP2350 225 MHz（QSPI 75 MHz，VREG 1.10V）；实测结论见 [`notes/architecture.md`](notes/architecture.md) |
| `PIO_USE_DMA` | `1` | 全刷 +12~16%，45 s 压测稳定；详见 [`notes/pitfalls-display.md`](notes/pitfalls-display.md) |
| 面板 | ILI9488 / 8080 并口 / PIO，320×480 原生（`TFT_ROTATION 1` → 480×320） | 改分辨率要连带改驱动分带与 QOI 缓冲上限；**面板参数由 `PUD_CMD_GET_CAPS` 上报**，主机不再写死 |
| 触摸采样 | 轮询 10 ms + EP4 `bInterval` 8 ms | 两个旋钮要一起改（主机只在 `bInterval` 到点时才来取报告）。实测拖动相邻点 32 ms → **8.0 ms（≈125 Hz）**，整屏吞吐无变化（A/B 四档 ±0.1%）；见 [`notes/touch-ep4.md`](notes/touch-ep4.md) |

## 可调构建开关（cache 变量，见 `CMakeLists.txt`）

| 开关 | 默认 | 作用 |
| --- | --- | --- |
| `PUD_DECODER_PINGPONG` | `1` | QOI/RLE 批次乒乓；关掉省 7680 B/解码器，代价是设备侧多花 7~39% 时间（[notes/qoi.md](notes/qoi.md)） |
| `PUD_CODEC_IN_RAM` | `1` | QOI/RLE 解码循环放 SRAM（编解码库的 `RGB565_*_SECTION` 钩子）；设备侧解码快 2~9%，链路受限时端到端无变化（[notes/architecture.md](notes/architecture.md)） |
| `QOI_NONCALLBACK` | `2` | QOI 走非回调 API + band 乒乓；设备侧比回调版快 22~48%，代价是 87348 B 缓冲（旧记约 87 KB，是旧公式没减 12 B EP1 头）；`0`/`1` 只用于 A/B 和回落（[notes/qoi.md](notes/qoi.md)） |
| `DECODER_STATS` | `0` | 解码/刷屏耗时计数器（`g_qoi_stat_*`），调试用 |
| `PUD_DELTA_WIN` | `32768`（RP2040 `16384`） | 只对 `DECODER_TYPE=6` 生效：**每个帧槽**一块字典窗口，布局 `[历史][输出]` ⇒ 单条带 QOI 与它 diff 的那条带各自不超过一半。RP2350 上 3 槽共 96 KB，`data+bss` 464 KB/520 KB ✓；超出的条带**计数丢弃**（`g_decoder_stat_qoid_oversize`），不截断 |
| `PUD_INFLATE` | `tinfl` | 只对 `DECODER_TYPE=5` 生效：`tinfl`（miniz）或 `libdeflate`；解同一种码流，**不是协议字段**。libdeflate 设备侧 inflate 快 32~45%，桌面负载端到端无变化（[notes/qoiz.md](notes/qoiz.md)） |

> 另有 `PUD_EP1_SINK`（默认 `0`，只测链路：收下 EP1 不解码不刷屏）与 `QOI_BUF_ROWS`（默认 `8`），
> 说明见 `CMakeLists.txt` 注释。

## 相关

- 知识库索引：[notes/README.md](notes/README.md)
- 通用知识库/测试/退出码/敏感信息约定：工作区根 [`../AGENTS.md`](../AGENTS.md)
- 协议权威定义：`PUD-kernel-drivers/notes/usb-protocol.md`（本仓那份是设备侧镜像）

## 驱动工具的方式（与工作区规范同源）

- **不许盲目 `sleep`，不许 blanket 超时** ✓ —— 用**轮询就绪**（0.2 s 间隔）+ **秒级超时** ✓。
  硬件测试必须**显式定义就绪检测**，不要依赖"设备恰好已经跑着" ✓。
- 反例：`sleep 22` + `timeout 300` ⇒ 明明 0.4 s 就有结论的操作拖到几分钟 ✗。
- 正解：`usb.core.find` 轮询 ✓、控制请求 0.5 s 超时 ✓、shell 命令 `timeout 10` ✓。
