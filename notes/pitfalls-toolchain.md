# 踩坑：工程、工具链与 USB 端点

> 最容易踩的是"默认按 RP2040 配置"；其次是生成的大数组 `bootlogo.h` 不能用批处理命令改。
> EP1 OUT 单缓冲是这块芯片上的合理选择，不是可以随手优化掉的东西。

## TL;DR

- 项目默认 `PICO_BOARD=pico`（RP2040）；裸 `cmake ..` 的固件烧到 Pico 2 上跑不起来，必须用 `build-pico2/`。
- `decoder_names[]` 必须覆盖所有 `DECODER_TYPE`（现 7 项，带 `_Static_assert`），编号不要重排。
- `include/bootlogo.h` 是 4500+ 行生成文件，**不要用 `sed -i`**。
- 改 `DECODER_TYPE` 后必须重新 `cmake`。
- EP1 OUT 继续单缓冲：CherryUSB 设备端没实现双缓冲，且实测设备从来不是丢包原因。

## 4.1 默认按 RP2040 配置（最容易踩）

项目默认 `PICO_BOARD=pico`（RP2040）。**直接 `mkdir build && cmake ..` 编出来的固件在 Pico 2
上跑不起来。**

```bash
# build/         → PICO_BOARD=pico   / PICO_PLATFORM=rp2040     ❌
# build-pico2/   → PICO_BOARD=pico2  / PICO_PLATFORM=rp2350-arm-s ✅
cd build-pico2 && cmake .. -DPICO_BOARD=pico2 && cmake --build . -j8
```

`PICO_PLATFORM=rp2350-arm-s` 表示用 **Cortex-M33（安全态）**，而不是 Hazard3 RISC-V。

## 4.2 `decoder_names[]` 必须包含所有类型

```c
/* src/decoders/decoder.c，索引即 DECODER_TYPE */
static char *decoder_names[] = { "tjpgd", "JPEGDEC", "LZ4", "QOI", "RLE",
                                 "QOI+deflate (tinfl)", "QOI+deflate+dict" };
_Static_assert(DECODER_TYPE < sizeof(decoder_names) / sizeof(decoder_names[0]),
               "decoder_names[] has to cover every DECODER_TYPE");
```

这个数组曾漏掉 `"QOI"`，而 `DECODER_TYPE=3` 会越界读。加解码器时同步这里；**编号不要重排**
（`decoder_type` 会通过 `PUD_CMD_GET_CAPS` 上报给主机）。`PUD_INFLATE=libdeflate` 时第 5 项
字符串不同（`QOI+deflate (libdeflate)`），数组长度不变。开机串口的 `Decoder type: QOI`
就来自这里，乱了说明数组有问题。

## 4.3 `include/bootlogo.h` 是个超大的条件编译文件

按 `DECODER_TYPE` 分支内嵌了多份 logo 压缩数据，4500+ 行。**不要用 `sed -i` 之类的命令式批处理
去改它** —— 曾因参数列表过长导致文件被清空，最后靠 `git checkout -- include/bootlogo.h`
才恢复。用编辑器的精确替换，或脚本内用 Python（`tools/mkbootlogo.py` 就是后者，落盘前还会
反解比对）。

## 4.4 WSL 里看不到调试器和 USB 设备

WSL 没有 `/dev/bus/usb`，也无法 `mknod` 造出来。所以：

- OpenOCD 必须在 **Windows 宿主机**跑，WSL 通过 `localhost:3333` 连；
- 主机侧（pyusb 之类）直接访问 Pico 在 WSL 里是做不到的；
- WSL 的 `/tmp` **每次命令调用是独立的**，别把中间产物放那儿再跨调用读。

（原生 Linux 开发机没有这一层，OpenOCD 与 gdb 都在本机，见 [debugging.md](debugging.md)。）

## 4.5 子模块拉取

`lib/` 下的直接子模块包括 `CherryUSB`、`lz4`、`pico-display-lib` 和 `FreeRTOS-Kernel`；
FreeRTOS 还包含两个 ports 子模块。必须 `--recursive`，漏了会在编译时报缺文件。

直连 GitHub 失败时，走代理并强制 HTTP/1.1 是验证过可行的组合：

```bash
export https_proxy=http://<proxy>:<port>
git -c http.version=HTTP/1.1 submodule update --init --recursive
```

`ghproxy`、`gitee` 镜像在本项目上验证**不可用**。

## 4.6 修改 `DECODER_TYPE` 后要重新 cmake

它是通过 `target_compile_definitions` 传进去的，只改 `.cmake` 不重新配置不会生效。
另外 `include/bootlogo.h` 的 logo 数据也随类型切换 —— 换了类型别忘了确认开机 logo 正常。

## 4.7 CherryUSB 子模块停在 v1.5.2，要不要升到 v1.6.1（2026-09 复核：不用）

我们只编译 `core/usbd_core.c` + `port/rp2040/usb_dc_rp2040.c` + 自己的 `usb.c`/`usbd_vendor.c`
（`src/cherryusb/CMakeLists.txt` 里显式列的）。**v1.5.2 → v1.6.1 一共动了 171 个文件，
但落在我们编译/包含范围内的只有 7 个**：

| 文件 | 变化 | 影响 |
| --- | --- | --- |
| `port/rp2040/usb_dc_rp2040.c` | **没变**（逐字节相同） | 无（USB 控制器行为不变） |
| `core/usbd_core.c` | 删掉非 ADVANCE_DESC 的旧路径；EP0 包长改为读设备描述符的 `bMaxPacketSize0`；`usbd_initialize/deinitialize` 清理（总线断言、EP0 mq 释放、新增 `USBD_EVENT_DEINIT`） | 我们描述符里 EP0 就是 64（宏里硬编码 `0x40`），行为一致 |
| `core/usbd_core.h` | `usbd_desc_register()` 成为唯一描述符 API（**ADVANCE_DESC 变强制**）；`usbd_initialize` 的 handler 加 typedef；多 include 两个新头 | 我们**已经**定义 `CONFIG_USBDEV_ADVANCE_DESC` ✓ |
| `common/usb_def.h` | BOS/WebUSB/WinUSB platform capability 结构体改名；注释缩进；描述符宏未动 | 我们不用这些描述符 |
| `common/usb_util.h` | `WBVAL/DBVAL` 加括号；新增 `DIV_ROUND_CLOSEST` | 更安全，无影响 |
| `common/usb_osal.h` | 多一个 `usb_osal_sem_create_counting` 原型 | 我们不用 OSAL |
| `common/usb_version.h` / `usb_otg.h` | 版本号；OTG mode 宏 | 无关 |

- 新增 `common/usb_ringbuffer.h`、`common/usb_mempool.h` 是**纯头文件**，而且 **device core
  里一次都没用到**（`grep` = 0），`CONFIG_USB_MEMPOOL_MAX_BLOCK_COUNT` 头里自带默认值 16
  → **配置不用加东西**。
- **端点路径一个字没动**：`usbd_ep_start_write/read`、`usbd_ep_close`、stall、`ep_cb`、
  `ep0_state` 在 diff 里都是零行 → 我们的 EP1/EP2/EP4 与 vendor 请求处理不受影响。
- 其余 160+ 个文件是 class/host/demo 与 DWC2/MUSB/EHCI 的改动 —— **都不在我们编译范围内**。

**结论**：升级是低风险的小事，但目前**没有任何功能收益**，先不动；等上游真的加了我们要的
东西（比如 device 侧双缓冲、或某个 core 修复）再升。真要升，记得"改完必须上板跑一遍"。

## 5.1 EP1 OUT 为什么不做双缓冲（2026-09，结论：做了也没用）

RP2040/RP2350 的 USB 控制器**支持**端点双缓冲（`EP_CTRL_DOUBLE_BUFFERED_BITS` +
`EP_CTRL_INTERRUPT_PER_DOUBLE_BUFFER` + `USB_BUF_CTRL_SEL`；RP2350 上同样的位在
`usb_device_dpram.h` 里叫 `USB_DEVICE_DPRAM_EPn_IN_CONTROL_DOUBLE_BUFFERED` /
`..._INTERRUPT_PER_DOUBLE_BUFF`），但我们的 bulk OUT **用不上、也不需要**：

- **CherryUSB 的设备端没实现**：`port/rp2040/usb_dc_rp2040.c` 给每个端点方向只分一个
  64 B DPRAM 缓冲（`next_buffer_ptr += 64`），PID 自己翻。**我们锁的 v1.5.2 和上游
  `master`（v1.6.1）这个文件逐字节相同**（`diff` 无输出），所以升级子模块也不会带来它；
  只有 **host** 端 `usb_hc_rp2040.c` 实现了双缓冲。
- **参考实现也故意避开 device OUT**：pico-sdk 里的 TinyUSB `dcd_rp2040.c` 有完整双缓冲代码，
  却对 device OUT 强制单缓冲，注释是 *"skip double buffered for OUT endpoint in Device mode,
  since host could send < 64 bytes and cause short packet on buffer0"*。社区有补丁
  （notro 的 gist）把 bulk 双向都打开，但没进上游。我们的传输**每次都短包结尾**，正好在那个坑上。
- **实测它本来就不是瓶颈**：同一个 55583 B 载荷、40 帧，真 QOI **1.130 MB/s**，
  等长的垃圾数据（设备完全不解码、每包 USB 工作量相同）**1.131 MB/s** —— 一样。
  也就是说设备从来不是丢包的原因；离全速理论上限（19×64 B/ms = 1.216 MB/s）差的那 7%
  在**总线/主机侧**（每帧能排下的 packet 数、URB 结构），双缓冲救不了。

**结论**：继续单缓冲；链路已经是这条路的天花板，要更快只能**少发字节**（更好的压缩、更小的脏区）。

## 相关

- 构建与烧录细节：[build-and-flash.md](build-and-flash.md)
- 内存约束：[pitfalls-memory.md](pitfalls-memory.md)
- 端点协议：[usb-protocol.md](usb-protocol.md)
