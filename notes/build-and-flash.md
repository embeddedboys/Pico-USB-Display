# 构建与烧录

> 唯一入口是仓库根的 `./build.sh`（实际脚本在 `scripts/`）；**RP2350 必须用 `build-pico2/`**，
> 直接 `mkdir build && cmake ..` 出来的是 RP2040 固件。

## TL;DR

- `./build.sh lunch` 选板子 + 面板配置 + 烧录方式（记在 `.pud-config`，已 gitignore），`./build.sh` 配置并构建，`./build.sh flash` 烧。
- `build/` = RP2040（`pico`），`build-pico2/` = RP2350（`pico2`）；本项目当前用后者。
- 面板配置来自仓库自己的 `configs/`（`-DPUD_CONFIG=<名字>`），不再是子模块那份。
- 编解码库统一加 `-ffunction-sections -fdata-sections`（不加以 `--gc-sections` 裁不掉压缩器，实测 text 112580 → 78988 B）。
- 烧完先按下面的"烧写与测量纪律"逐项确认，再开批量。

## 前置依赖

```bash
sudo apt install cmake python3 build-essential \
     gcc-arm-none-eabi libnewlib-arm-none-eabi \
     libstdc++-arm-none-eabi-newlib ninja-build

git clone https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
cd ~/pico-sdk && git submodule update --init
```

## 编解码库必须带 `-ffunction-sections`

`src/decoders/CMakeLists.txt` 给 jpegdec/tjpgd/lz4/qoi/rle 统一加了
`-ffunction-sections -fdata-sections`。原因是这些库都是**一个 .c 同时带压缩和解压两个方向**，
而固件只用其中一个；没有 function sections 时 `--gc-sections` 无法按函数裁剪，只能整块保留。

实测（LZ4 构型，480×320 logo）：加上之前镜像里有 33232 B 的 `LZ4_*` 代码（压缩器、streaming
API、字典函数全在），实际用到的只有 `LZ4_decompress_safe` 一个；加上之后 **33232 → 1012 B**，
整机 `text` 从 112580 降到 78988 B。加新编解码库时保持这两个 flag。

子模块（`lib/CherryUSB`、`lib/lz4`、`lib/pico-display-lib`、`lib/FreeRTOS-Kernel` 及其
`Community-Supported-Ports`/`Partner-Supported-Ports`）都要拉全：

```bash
git submodule update --init --recursive
```

> 若直连 GitHub 失败，可走代理后强制 HTTP/1.1：
> ```bash
> export https_proxy=http://<proxy>:<port>
> git -c http.version=HTTP/1.1 submodule update --init --recursive
> ```
> （`ghproxy`/`gitee` 镜像在本项目上验证过不可用。）

## 构建前端：`./build.sh` 与 `lunch`

```bash
./build.sh lunch                       # 选板子 + 选面板配置 + 烧录方式，记在 .pud-config
./build.sh                             # 按所选构建
./build.sh -b pico2 -c generic-st7789v # 临时换目标，不改 .pud-config
./build.sh -j8                         # 只改并行度（-j8 / -j 8 / --jobs=8 都认）
./build.sh config                      # 打印当前选择
./build.sh config <名字>               # 某个配置的完整字段
./build.sh configs                     # 列出 configs/ 下所有配置（一行一个）
./build.sh clean                       # 删掉所选板子的构建目录
```

- **`lunch` 只选不编**：板子（`pico` → `build/`，`pico2` → `build-pico2/`）和面板配置
  （`configs/*.cmake`），**回车 = 保持当前值**；结果写进 `.pud-config`（`BOARD=`/`CONFIG=`/
  `FLASH=` 几行，**不提交**，已 gitignore），之后 `./build.sh` 都照它来。没有 `.pud-config`
  时用 `CMakeLists.txt` 里的默认值。
- **配置来自本仓库的 `configs/`**，通过 `-DPUD_CONFIG=<名字>` 传给 CMake；
  `lib/pico-display-lib/configs/` 是上游来源，改本机面板参数不必动子模块。裸 `cmake -S . -B build`
  不受影响，仍走 `CMakeLists.txt` 的默认值。
- 构建目录按板子分开，**不会**因为换配置而共用。
- 脚本自己找 SDK（`$PICO_SDK_PATH` → `~/.pico-sdk` → `~/pico/pico-sdk` → `/opt/pico-sdk`），
  找不到就直接报错，不做猜测。

## 构建目录：**必须用 RP2350 的那个**

| 目录 | `PICO_BOARD` | `PICO_PLATFORM` | 说明 |
| --- | --- | --- | --- |
| `build/` | `pico` | `rp2040` | ⚠️ **RP2040**，默认会选错 |
| `build-pico2/` | `pico2` | `rp2350-arm-s` | ✅ **RP2350**，本项目当前使用 |

```bash
cd build-pico2
cmake .. -DPICO_BOARD=pico2          # 首次配置
cmake --build . -j8
```

（与 `./build.sh -b pico2` 完全等价 —— 后者就是这两条 + 前面选好的 `-DPUD_CONFIG`。）

产物：`pico-usb-display.elf`（gdb/OpenOCD 烧录与调试，带符号）、`pico-usb-display.uf2`
（拖到 USB 大容量盘烧录）。`PICO_PLATFORM=rp2350-arm-s` 表示用 **Cortex-M33（安全态）**，
而不是 Hazard3 RISC-V。

CMake 还提供 `flash` 目标（按板子选 `target/rp2040.cfg` 或 `target/rp2350.cfg`），它要求
**openocd 与构建在同一台机器上** —— 原生 Linux 开发机上可以直接
`cmake --build build-pico2 --target flash`；只有 OpenOCD 跑在 Windows 宿主机那套接法下，
才要改用下面的 gdb 方式。

## 编辑器 / clangd

`.clang-format`、`.clangd`、`.vscode/`、`.editorconfig` 都在仓库根，开箱可用 ——
**前提是先 configure 过一次**（`compile_commands.json` 由 CMake 写进构建目录）。

- 编辑器**不写死任何构建目录**：`.clangd` 与 `.vscode/settings.json` 都不指定路径，clangd 靠
  自己的发现规则去找 `compile_commands.json`（先搜源文件所在的树，再搜 `build/`）。
  `build-pico2/` 不在默认范围里，所以 `CMakeLists.txt` 在每次 **configure** 时把仓库根的
  `compile_commands.json` **符号链接**指向该构建目录（只替换链接，绝不动真实文件）：
  `build-pico2/` 和 `build/` 谁最后 configure，clangd 就用谁的参数（链接是生成物，已 gitignore）。
  注意 clangd 也会把 `build/` 当候选：链接不在时它会**悄悄**用 RP2040 的参数，那比没有索引更
  容易误导；删掉 `build-pico2/` 之后重新 configure 即恢复。
- 编译命令调用的是 `arm-none-eabi-gcc`，clangd 得向它索取内建头文件路径，否则会报找不到
  `stdint.h`：VS Code 已传 `--query-driver=**/arm-none-eabi-*`；命令行单跑用
  `clangd --check=src/cherryusb/usb.c --query-driver=/usr/bin/arm-none-eabi-*`
  （结尾的 `All checks completed, 0 errors` 就是通过）。
- 风格是**内核风格**（tab + 8 宽，`.clang-format` 取自内核，唯一偏离是
  `UseTab: ForIndentation`），保存即格式化。**vendored 与生成的代码不吃这套**：
  `src/decoders/{qoi,rle,jpegdec,tjpgd}/` 各放一份 `DisableFormat: true` 的 `.clang-format`；
  `include/bootlogo.h`、`tools/stb_*.h`、`FreeRTOSConfig.h`、`src/cherryusb/usb_config.h` 里写了
  `// clang-format off` —— 后两个是为了保住 `#define` 选项的列对齐。

## 烧录（`./build.sh flash`）

```bash
./build.sh lunch                   # 板子 / 面板配置 / 烧录方式
./build.sh flash                   # 按选中的方式烧 build*/ 里的产物
./build.sh flash -m blackmagic     # 这次换一种方式
./build.sh flash -n                # 只打印命令行，什么都不执行
./build.sh flash -t /dev/ttyACM1   # 换 GDB server / 探针串口
```

| `FLASH=` | 方式 | 命令（脚本里就是这条） | 状态 |
| --- | --- | --- | --- |
| `picotool` | 板子在 BOOTSEL（应用态也能自己送进去），走 USB，本机烧 `build*/*.uf2` | `picotool load -v -x <uf2>` | ✅ **实测**（2026-09-27，RP2040）：`Loading into Flash` 0→100%、`Verifying Flash` 0→100%、`OK`、随后枚举回 `2e8a:0001`；**应用态直接烧**（`-m picotool --reboot`）也实测通过 |
| `openocd` | 本机 openocd + CMSIS-DAP 直连（Debug Probe） | `openocd -f interface/cmsis-dap.cfg -c "adapter speed 10000" -f target/rp2040.cfg -c "program <elf> verify reset exit"` | ✅ **实测**（2026-09-27，openocd 0.12.0+dev / RP2040 rev 2 / 4 MB flash）：`** Verified OK **`，exit 0 |
| `gdb` | gdb 连一个已经在跑的 GDB server（OpenOCD，默认 `localhost:3333`） | 下面那段实测命令 | ✅ **实测**（2026-09-26 在 Windows 宿主 OpenOCD 上；2026-09-27 又用本机 Debug Probe + 本机 openocd 复测：`load size 113300`、`73 KB/sec`） |
| `blackmagic` | 黑魔法探针：gdb 直连它自带的 GDB server（默认 `/dev/ttyACM0`） | `gdb ... -ex "target extended-remote /dev/ttyACM0" -ex "monitor swdp_scan" -ex "attach 1" -ex "load" -ex "compare-sections" -ex "kill"` | **未验证**（手头没有 BMP） |
| `none` | 只构建，自己拖 uf2 | — | 拷贝 uf2 到 `RPI-RP2` 挂载盘这条路实测过（225 KB 约 1.3 s） |

- 目标选择跟着板子走：`pico` → `build/` + `target/rp2040.cfg`，`pico2` → `build-pico2/` +
  `target/rp2350.cfg`（和 CMake 的 `flash` 目标一致）。
- gdb 用哪个二进制：依次找 `gdb-multiarch` → `arm-none-eabi-gdb` → `gdb`。**架构支持是 gdb 自己
  带的**，2026-09-27 实测本机只有 `/usr/bin/gdb` 也能烧。工具路径可用 `PUD_GDB` /
  `PUD_PICOTOOL` / `PUD_OPENOCD` 覆盖；工具不在本机时 `-n` 照样打印命令行。
- `picotool` 这一路要求板子**已经在 BOOTSEL**：按住 BOOTSEL 插拔，或者用
  `./build.sh flash --reboot` 让脚本自己把它送进去（实测 2026-09-27，从应用态一路烧完）：
  - 固件带 **picoboot 的 reset 接口**（配置描述符里的第 2 个接口，`0xFF/0x00/0x01`，见
    [usb-protocol.md](usb-protocol.md)），所以 picotool 可以直接请**正在跑**的板子重启：
    `picotool reboot --vid 0x2e8a --pid 0x0001 -f -u` → 板子随即变成 `2e8a:0003`。两个坑：
    `--vid/--pid` **必须显式给**；设备选择选项要写在命令自己的选项**前面**。
  - 这条不通时（旧固件 / 没权限）退回调试器：调 RP2040 bootrom 的 `reset_usb_boot`（ROM 函数
    `UB`）。它**在调用里就复位芯片**，所以 openocd 必定报 `Failed to call ROM function batch /
    ROM API call failed` —— 那是正常的，等 2 s `lsusb` 里就会出现 `2e8a:0003`。
    RP2350 没有这条（重启 API 是 `RB`，标志位没验证过）。
  ```bash
  picotool reboot --vid 0x2e8a --pid 0x0001 -f -u
  ```
  调试器退路：调 RP2040 bootrom 的 `reset_usb_boot`（ROM 函数 `UB`，openocd 需带
  `rom_api_call` 的 0.12.0+dev）。它**在调用里就复位芯片**，所以 openocd 必定报
  `Failed to call ROM function batch / ROM API call failed` —— 那是正常的，等 2 s `lsusb` 里
  就会出现 `2e8a:0003`。RP2350 没有这条（重启 API 是 `RB`，标志位没验证过）。
- 它还要**打得开 BOOTSEL 设备**：udev 规则必须放行 bootrom（`2e8a:0003`，RP2350 是 `000f`）。
  仓库的 `60-pico-usb-display.rules` 已经放行了面板、两个 bootrom，外加 SDK `stdio_usb` 的
  `0009`/`000a`；装完重新插拔设备。装好后可用 `udevadm test /sys/bus/usb/devices/<路径> | grep MODE`
  确认它真的匹配到了：出现 `60-pico-usb-display.rules` 那行就说明生效（默认规则先给 `0664`，
  **靠这条才盖成 `0666`** —— 文件名前缀低于 `50-` 时连这行都不会有）。
- BOOTSEL 下板子就是一块 **USB 大容量盘**：不想装 picotool 时，把 `build*/*.uf2` 拷进自动挂载的
  `RPI-RP2`（或 `RP2350`）卷、等它卸载就烧完了，实测 225 KB 约 1.3 s。`FLASH=none` 就是这个意思。
- `gdb` 与 `blackmagic` 的区别只在连谁：前者连别人跑着的 OpenOCD，后者连探针自己的 GDB server，
  并且多一段 `monitor swdp_scan` / `attach 1`。

### 手工：宿主机 OpenOCD + gdb（实测）

先在能看到调试器的机器上启动 OpenOCD：

```bash
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "adapter speed 10000"
```

再在开发机上用 gdb 连 `localhost:3333`：

```bash
gdb-multiarch -q -nh \
  -ex "set pagination off" -ex "set confirm off" \
  -ex "file pico-usb-display.elf" \
  -ex "target extended-remote localhost:3333" \
  -ex "monitor reset halt" -ex "load" \
  -ex "monitor reset run" -ex "detach" -ex "quit"
```

> `-q -nh` 是必要的：不加会去读 `~/.gdbinit`，某些环境下（如装了 gef）会报错中断脚本。
> 项目里的 `.gdbinit` 只有一行 `target extended-remote :3333`。

**原生 Linux 上实测（2026-09-26）**：上面这条原样可用 —— 各段 `.text/.rodata/.data/.noncacheable`
全部写入，`load size 154376`、`Transfer rate 63 KB/sec`；烧完主机看到干净的重新枚举，
`PUD_CMD_GET_CAPS` 读回的 caps 与烧录前**逐字段一致**。成功输出形如：

```
Loading section .text, size 0xce40 lma 0x10000000
Loading section .noncacheable, size 0x204fc lma 0x10015890
...
Start address 0x1000014c, load size 220576
```

### 为什么 `picotool` 必须带 `--vid/--pid`

不是我们的问题，是 picotool 的设备过滤（`picoboot_open_device()`）：它在**打开设备之前**先按
PID 分桶，表里只有

```
0x0003 RP2040 USB boot   0x0004 picoprobe   0x0005 micropython
0x0009 RP2350 SDK CDC    0x000a RP2040 SDK CDC   0x000f RP2350 USB boot
```

其它 PID 一律 `return dr_vidpid_unknown` —— 而"扫 reset 接口"的代码在这个 `switch` **之后**，
所以 `2e8a:0001` 根本走不到它。实测（2026-09-27，板子在应用态）：

| 写法 | 结果 |
| --- | --- |
| `picotool reboot -f -u` | ✗ `No accessible RP-series devices in BOOTSEL mode were found.` |
| `picotool reboot --pid 0x0001 -f -u` | ✅ `The device was asked to reboot into BOOTSEL mode.` |
| `picotool reboot --vid 0 -f -u` | ✅ 同上（`--vid 0` = 完全不过滤） |

`scripts/flash.sh` 因此固定带上 `--vid 0x2e8a --pid 0x0001`（`PUD_VID`/`PUD_PID` 可覆盖）。

**要让默认过滤也认（一次都不带参数），只有改 PID 一条路**：`0x000a`（RP2040）/`0x0009`
（RP2350）是表里唯一会落进 `dr_vidpid_stdio_usb` 桶的取值。**2026-09-27 决定不改**，因为代价
不小而收益只是省掉脚本内部一个参数：驱动 `pud_ids[]`、`tools/pud_usb.py` 的 VID/PID、两个仓库
的文档都要跟着改；已有部署是破坏性的（新固件 + 老驱动不匹配）；语义上顶着"Pico SDK CDC"的
身份，同插两个时 `picotool -f` 只能靠 `--ser` 选。

## 烧写与测量纪律（真机，血泪换的）

1. **批量前先跑一个点**，逐项确认：①构建 ✓ ②**回读 flash 与构建产物比对** ✓ ③拿到日志/结果 ✓
   ④状态行里的**实测频率 = 请求值** ✓。四项齐了再开循环 —— 曾整批死在烧写上白等半小时。
2. **`openocd program ... verify` 可能报 "Verified OK" 却只写了一部分** ✗（实测：flash 从
   0x800 起仍是 0xFF，板子还在跑旧镜像，看起来像新固件行为异常）。要么先 `flash erase_sector`
   再写，要么用 picotool，并且**回读比对**才算数。
3. **调试会话结束时核不能留在 halt** ✗：核停了，bootrom 的 USB 也不上线，板子会从 `lsusb`
   整个消失，下一次烧写报 "no accessible RP-series devices"。会话必须以 `reset run`/`resume`
   收尾。
4. **日志读取器要在烧写之前启动** ✓：否则抓到的是上一个应用的残留输出，读起来像是这次成功了。
5. **过快的 flash 分频会写进 boot2**，于是每次复位都重演同一个失败 —— 实测官方 Pico 2 在
   520 MHz 配 DIV 4（130 MHz flash）时核进 lockup，**软件复位救不回来，只能按 BOOTSEL**。
   做分频实验时手边要够得着按键。
6. **调试器是定位工具，别为了"纯 USB"丢掉它**：PC 直接说明状态（在函数里 = 在跑；
   `isr_hardfault` = 真挂了；在 bootrom = 镜像没起来）。
7. **工具输出不要静默**（`>/dev/null` 会把真正的错误一起吞掉 ✗），关键步骤把结果打出来。
8. **不要把脚本的"模型列"当实测**（细节见 [scripts-measurements.md](scripts-measurements.md) 与
   [codec-selection.md](codec-selection.md) 的"测量纠正"）：
   只有带 `median`/`bandwidth`/`encode` 的 `device_table` 才是设备实测；绝对速率是会话属性，
   只有比值能搬。

9. **`openocd ... program ... reset exit` 之后，设备可能"枚举正常但完全不应答控制请求"** ✗
   实测：`lsusb` 看得到 `2e8a:0001`、`probe` 也找得到，但任何 vendor 请求都返回
   `[Errno 5] Input/Output Error`；**数分钟后仍不应答**，而内核日志里没有重复枚举 —— 所以它
   不是在复位循环里。**物理重新插拔即可恢复**，恢复后 `pudctl caps` 一切正常。固件本身没问题
   （同一镜像此前在同一端口正常应答过），是这个复位流程把芯片留在了那个状态。遇到"设备在、
   但不答应"先插拔一次，再怀疑固件。同类现象也可能只是应用还没开始服务请求，`pud_usb.wait_ready()`
   会重试一段时间再下结论。

## 配置项

### `CMakeLists.txt`

| 配置 | 当前值 | 说明 |
| --- | --- | --- |
| 显示配置 include | `pico_dm_qd3503728.cmake` | 决定屏型号/分辨率/引脚 |
| `OVERCLOCK_ENABLED` | `1` | 板配置 profile 1：RP2350 225 MHz / QSPI 75 MHz / 1.10V，见 [architecture.md](architecture.md) |
| `PIO_USE_DMA` | `1` | I8080 走 PIO + DMA（早期 DREQ 卡死已复测通过，见 [pitfalls-display.md](pitfalls-display.md)） |
| `DECODER_TYPE` | `3` | 3 = QOI（其余类型与默认值见 [../AGENTS.md](../AGENTS.md)） |

（不写 `CMakeLists.txt` 的行号了 —— 加一行就漂，已经漂过一次。）

### 显示配置（`configs/*.cmake`）

仓库自己的 `configs/` 下每个面板一个 `.cmake`（20 个），由 `-DPUD_CONFIG=<名字>` 选中。
常用：`pico_dm_qd3503728`（当前面板）、`pico_dm_yt350s006`、`generic-ili9341`、`generic-st7789v`；
其余见 `./build.sh configs`。`lib/pico-display-lib/configs/` 是同一批文件的**上游来源**，构建不再
include 它 —— 改本机面板参数改仓库这份即可，不会在子模块更新时被覆盖。

关键变量（以当前生效的 qd3503728 为例）：

```cmake
set(TFT_BUS_TYPE 1)          # 1 = 8080 并口
set(TFT_PIN_DB_COUNT 16)     # 16 位数据总线
set(TFT_HOR_RES 320)
set(TFT_VER_RES 480)
set(TFT_ROTATION 1)          # 90° → 主机看到 480x320
set(DISP_OVER_PIO 1)
set(TFT_DRV_USE_ILI9488 1)
set(INDEV_DRV_USE_FT6236 1)  # 触摸控制器
```

**改了分辨率要同步检查**：① 驱动侧的分带尺寸（`pud->max_band_pixels`，由 `PUD_CMD_GET_CAPS`
从设备得到）与 `tx_buf` 大小；② 固件的 QOI 缓冲尺寸（`qoi_buf_a/b[480 * QOI_BUF_ROWS]` 里的
480 是硬编码上限）；③ `include/bootlogo.h` 里的 logo 数据是按分辨率编好的。

## 增量构建的注意点

- `DECODER_TYPE` 改了之后要重新 `cmake ..`（它是 `target_compile_definitions` 传入的）。
- `include/bootlogo.h` 按解码器类型分支，改 `DECODER_TYPE` 会切换整个数组 —— 文件很大
  （4500+ 行），**不要用会截断文件的命令**（曾用 `sed` 处理时因参数列表过长把文件清空，
  最后靠 `git checkout -- include/bootlogo.h` 恢复）。
- 构建产物（`build*/`）在 `.gitignore` 里，不要提交。

## 相关

- 烧录后的调试（gdb、HardFault、计数器）：[debugging.md](debugging.md)
- 时钟/主频与启动流程：[architecture.md](architecture.md)
