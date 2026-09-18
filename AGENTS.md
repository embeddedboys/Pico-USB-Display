# AGENTS.md

本仓库的工作规则，供 AI agent（以及人）在改动前先读一遍。

**详细知识在 [`notes/`](notes/README.md)**：本文只写"必须遵守的约束"和入口，
不重复细节，以免每次会话都吃掉大量上下文。

---

## 铁律

1. **未经明确指令，不要 `git commit`，更不要 `git push`。**
   改完先报告改了什么、验证到什么程度，等指令。
   （曾经把"告诉你提交者身份"误解成"让你提交"，多做了事。）
2. **构建必须用 `build-pico2/`**（`PICO_BOARD=pico2` / RP2350）。
   仓库根的 `build/` 是 RP2040 配置，烧到 Pico 2 上跑不起来。
3. **仓库内不得出现内网/个人信息**：本机绝对路径、内网 IP、口令、内部代号。
4. **不要把解码放进 USB 中断**（会 HardFault，见"架构不变量"）。
5. **不要去掉 EP1 流控**（那是局部刷新残影的修法，见"架构不变量"）。

## 提交与身份

- `user.name` = `Wooden Chair`，`user.email` = `hua.zheng@embeddedboys.com`
- **提交一律带 `Signed-off-by`**：用 `git commit -s`（仓库既有历史都带 sign-off）
- 提交信息用**内核风格**：`模块: 组件: 简述`，正文写清具体改了什么、为什么、效果；
  一个逻辑改动一个提交，不要把互不相关的改动塞进同一个提交
- 默认分支 `main`；子模块指针改动要和子模块提交一起考虑

## 构建与烧录

```bash
cd build-pico2 && cmake .. -DPICO_BOARD=pico2 && cmake --build . -j8
```

- 子模块要 `--recursive`（CherryUSB / lz4 / pico-display-lib / FreeRTOS-Kernel
  及其 ports）。直连 GitHub 失败时，"走代理 + `git -c http.version=HTTP/1.1`"
  是验证过可行的组合（`ghproxy`/`gitee` 镜像不可用）。
- 烧录：OpenOCD 跑在 **Windows 宿主机**（WSL 看不到调试器，也无法 `mknod`
  出 `/dev/bus/usb`），WSL 侧用
  `gdb-multiarch -q -nh -ex "target extended-remote localhost:3333"`。
  `-q -nh` 是必需的（否则会读 `~/.gdbinit`，装了 gef 之类会直接报错中断）。
- **只读检查固件状态时，读完要 `monitor resume`**；别用 `monitor reset run`
  （会清掉计数器和显示状态）。卡死时复位才用它。
- 细节见 [`notes/build-and-flash.md`](notes/build-and-flash.md) 与
  [`notes/debugging.md`](notes/debugging.md)。

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
   判定标准：`g_decoder_stat_dropped == 0`，且 `drawn` 落后 `submitted` 恰好 1 帧。
   去掉它 = 槽满静默丢帧 = 局部刷新残影。
3. **RAM 很紧。** RP2350 512 KB SRAM 里 `ep1_read_buffer`（64 KB）+
   `s_frames`（2 × 64 KB）已经占掉一大块；**RP2040 只有 256 KB 可用**，
   所以这两个尺寸**都不是写死的，由 `PUD_MAX_TRANSFER` 按板子决定**
   （`src/cherryusb/usbd_vendor.h`，RP2350 64 KB / RP2040 32 KB），
   帧槽在 `decoder.c` 里用同一个宏并有 `_Static_assert` 兜底。
   改它 = 改协议，主机靠 `PUD_CMD_GET_CAPS` 问设备（见"架构不变量"第 6 条）。
   **不要把 `DECODER_FRAME_SLOTS` 或 `PUD_MAX_TRANSFER` 翻倍**
   （RP2040 上实测 128 KB + 2×64 KB 时 .data/.bss 达到 RAM 的 109%，直接链接失败）。
4. **`configTOTAL_HEAP_SIZE` 在本项目不起作用** —— 链接的是 `heap_3.c`，
   它只包装 `malloc`。想限制堆得改链接脚本或换 heap_4。
5. **`decoder_names[]` 必须覆盖所有 `DECODER_TYPE`**（曾漏 `"QOI"` 导致越界读），
   且**不要把编号重排** —— `decoder_type` 会通过 `PUD_CMD_GET_CAPS` 上报给主机。
   两种 JPEG 实现都保留：tjpgd（局刷正确但慢）/ JPEGDEC（快但 `x != 0` 会卡死显示），
   见 [notes/decoders.md](notes/decoders.md)。
6. **协议字段改动要成对改驱动**（`REQ_*`、`struct pud_ep1_header`、`struct req_ep2_in`），
   并同步两个仓库的 `notes/usb-protocol.md`。
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
   （`lz4_band[43680]`，按主机分带用的同一个 `band_pixels` 规则定尺寸），
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

## 当前配置（改前先读 notes）

| 配置 | 值 | 说明 |
| --- | --- | --- |
| `DECODER_TYPE` | `3`（QOI） | 图片/视频脚本按 QOI 发；`0`=tjpgd、`1`=JPEGDEC、`2`=LZ4、`4`=RLE。**编号是协议字段**（`PUD_CMD_GET_CAPS` 上报），不要重排 |
| `OVERCLOCK_ENABLED` | `1` | 板配置 profile 1：RP2350 225 MHz（QSPI 75 MHz，VREG 1.10V）；实测结论见 [`notes/architecture.md`](notes/architecture.md) |
| `PIO_USE_DMA` | `1` | 全刷 +12~16%，45 s 压测稳定；详见 [`notes/pitfalls.md`](notes/pitfalls.md) |
| 面板 | ILI9488 / 8080 并口 / PIO，480×320（旋转后） | 改分辨率要连带改驱动分带与 QOI 缓冲上限；**面板参数由 `PUD_CMD_GET_CAPS` 上报**，主机不再写死 |
| 触摸采样 | 轮询 10 ms + EP4 `bInterval` 8 ms | 两个旋钮要一起改（主机只在 `bInterval` 到点时才来取报告）。实测拖动相邻点 32 ms → **8.0 ms（≈125 Hz）**，整屏吞吐无变化（A/B 四档 ±0.1%）；见 [`notes/usb-protocol.md`](notes/usb-protocol.md) 的 EP4 一节 |

可调构建开关（cache 变量，见 `CMakeLists.txt`）：

| 开关 | 默认 | 作用 |
| --- | --- | --- |
| `PUD_DECODER_PINGPONG` | `1` | QOI/RLE 批次乒乓；关掉省 7680 B/解码器，代价是设备侧多花 7~39% 时间（[decoders.md](notes/decoders.md)） |
| `PUD_CODEC_IN_RAM` | `1` | QOI/RLE 解码循环放 SRAM（编解码库的 `RGB565_*_SECTION` 钩子）；设备侧解码快 2~9%，链路受限时端到端无变化（[architecture.md](notes/architecture.md)） |
| `QOI_NONCALLBACK` | `2` | QOI 走非回调 API + band 乒乓；设备侧比回调版快 22~48%，代价是 87 KB 缓冲（`0`/`1` 只用于 A/B 和回落，[decoders.md](notes/decoders.md)） |
| `DECODER_STATS` | `0` | 解码/刷屏耗时计数器（`g_qoi_stat_*`），调试用 |

## 用户空间工具（`scripts/` 与 `tools/`）

- **不加载内核驱动就能验证全部功能**（pyusb 直连），比反复 insmod/rmmod 快得多。
  这是首选的验证方式。
- **依赖选型**：`pyusb` + `Pillow`（≈3 MB，用来替代 `opencv-python` 的 ≈60 MB）；
  视频/录屏用 `ffmpeg` CLI；`numpy` **可选**（只影响 RGB565 打包速度）。
- **每种编码器只保留一份**，都在 `scripts/pud_usb.py`（`ENCODERS`）：QOI 与 RLE 各自与
  它们的 C 库**逐字节一致**（`python3 scripts/pud_usb.py` 自检里有参考向量），LZ4 用
  `lz4.block`（就是内核链接的那份 liblz4；它的码流**跨版本不保证逐字节一致**，但都能解）。
  新脚本必须复用它们，不要再写第二份编码器或第二套协议常量。
  发图统一走 `Display.send_rgb565(..., codec=...)`，**分带由它负责**（LZ4 尤其不能整帧发）。
- `tools/pudcodec` 是 C 写的**离线**转换器（图片/帧序列 ↔ 码流），编解码类型运行时用
  `--codec` 指定；构建 `cmake -S tools -B tools/build`，`stb` 已 vendor 不需要联网。
  它与 `pud_usb.py` 在无损源上**逐字节一致**，用 `scripts/check_pudcodec.py` 对拍。
  `--codec lz4` 输出的是 **band 容器**（每 band 一个 block，`--band` 默认取能整除高度的
  最大行数），因为整帧 block 设备解不了（见"架构不变量"第 9 条）。
- 本项目面向**桌面**（配合 DRM 驱动），主负载是**局部刷新**：评估编解码器用
  `scripts/desktop_codecs.py`（按"桌面会脏的矩形"比较），整屏照片/噪声测试**不代表**它。
  真实桌面内容上的结论是 **QOI 每个矩形都快 21~32%**（载荷少 15~35%，三者都跑在
  1.0~1.1 MB/s 的链路极限上）；LZ4 的优势在内核侧（不用 vendor 编码器），不是性能
  （见 [notes/decoders.md](notes/decoders.md)）。**给设备计时必须把编码放在循环外**，
  否则量的是 Python 编码器而不是解码器。无间隔连发小矩形**不会**打挂板子（2026-09
  无调试器复测，见 [notes/todo.md](notes/todo.md) 第 8 条），脚本 `--gap-ms` 默认 0。
- 设备必须未被 `pud` 驱动占用；装 `60-pico-usb-display.rules` 可免 root。
  **文件名里的 `60-` 不能退回 `50-`** —— 会被
  `/usr/lib/udev/rules.d/50-udev-default.rules` 覆盖而完全失效。
- 用法与实测数据见 [`notes/scripts.md`](notes/scripts.md)。

## 代码约定

- C 风格见 `.clang-format`；`u8/u16/u32` 是本仓库的类型别名。
- 新增源文件/目录要加进对应的 `CMakeLists.txt`（`PUD_SOURCES` 或子目录）。
- **调试打印要算代价**：115200 波特下每行约 1~3 ms，
  **不要放进每帧路径**。已知例子：EP2 查询路径的 `usb_hexdump` + `USB_LOG_WRN`
  实测 **9.6 ms/次**（`lz4_drawimg()` 也曾每帧 3 行 `printf`，约 10 ms，已随 LZ4 重写删掉）。
- 大块缓冲不要每帧 `malloc`（LZ4 曾每帧申请 ~307 KB，已改成静态 band 缓冲；
  解码器一律用静态缓冲或调用方缓冲）。
- `src/decoders/qoi/` 与 `src/decoders/rle/` 是 **vendored 代码，必须与上游仓库
  （`rgb565-qoi` / `rgb565-rle`）逐字节一致**：要改行为先改上游，再把两个文件整体拷回来
  （`cmp` 验证）。**编译期差异走它们的钩子**，不要在 vendored 文件里塞本项目的改动 ——
  已经有的例子是 `RGB565_QOI_SECTION` / `RGB565_RLE_SECTION`（放置钩子，见
  [architecture.md](notes/architecture.md) 的 SRAM 一节）。
- **TFT 像素格式只有一个出处**：驱动 init 里的 `0x3A`（COLMOD）。`tft_video_sync()` 和异步
  路径都原样把缓冲区送出去，**不要再给驱动加"顺手转 RGB666"的 `video_sync`**（ILI9488/9486
  里那两份 0x55 却转 3 字节的已经删了；ILI9481 保留是因为它 init 就是 `0x66`）。
  详见 [pitfalls.md](notes/pitfalls.md) 的 2.4。

## 文档维护

- 知识库在 [`notes/`](notes/README.md)：架构、协议、解码器流水线、构建烧录、
  调试、用户空间脚本、踩坑。
- 改了行为就同步对应文档；协议改动要**同时**改驱动仓的镜像文档。
- **只写已验证的结论**；推测显式标注"未验证"。
- 文档用中文，命令/路径/标识符保留英文。
- `README.md` 是中文主文档，`README.en.md` 是英文镜像 —— 改一个就改另一个。
