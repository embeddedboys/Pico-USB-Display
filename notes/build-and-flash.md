# 构建与烧录

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
`-ffunction-sections -fdata-sections`。原因是这些库都是**一个 .c 同时带压缩和解压两个
方向**，而固件只用其中一个；没有 function sections 时 `--gc-sections` 无法按函数裁剪，
只能整块保留。

实测（LZ4 构型，480×320 logo）：加上之前镜像里有 33232 B 的 `LZ4_*` 代码（压缩器、
streaming API、字典函数全在），实际用到的只有 `LZ4_decompress_safe` 一个；加上之后
**33232 → 1012 B**，整机 `text` 从 112580 降到 78988 B。加新编解码库时保持这两个 flag。

子模块（`lib/CherryUSB`、`lib/lz4`、`lib/pico-display-lib`，以及它们自己的嵌套子模块
`FreeRTOS-Kernel` + `Community-Supported-Ports`/`Partner-Supported-Ports`）都要拉全：

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

根目录的 `build.sh` 是唯一入口，实际干活的脚本都在 `scripts/`
（`lunch.sh` 选目标、`build.sh` 配置并构建、`config-info.sh` 描述一个配置）：

```bash
./build.sh lunch                       # 选板子 + 选面板配置，记在 .pud-config
./build.sh                             # 按所选构建（等价于下面的 cmake + 构建两条命令）
./build.sh -b pico2 -c generic-st7789v # 临时换目标，不改 .pud-config
./build.sh -j8                         # 只改并行度（-j8 / -j 8 / --jobs=8 都认）
./build.sh config                      # 打印当前选择
./build.sh config <名字>               # 某个配置的完整字段
./build.sh configs                     # 列出 configs/ 下所有配置（一行一个：面板/总线/分辨率/触摸）
./build.sh clean                       # 删掉所选板子的构建目录
```

- **`lunch` 只选不编**：板子（`pico` → `build/`，`pico2` → `build-pico2/`）和面板配置
  （`configs/*.cmake`），**回车 = 保持当前值**；结果写进 `.pud-config`
  （`BOARD=`/`CONFIG=` 两行，**不提交**，已 gitignore），之后 `./build.sh` 都照它来。
  没有 `.pud-config` 时用 `CMakeLists.txt` 里的默认值。
- **配置来自本仓库的 `configs/`**，通过 `-DPUD_CONFIG=<名字>` 传给 CMake；
  `lib/pico-display-lib/configs/` 是上游来源，改本机面板参数不必动子模块。
  裸 `cmake -S . -B build` 不受影响，仍走 `CMakeLists.txt` 的默认值。
- 构建目录按板子分开，**不会**因为换配置而共用：换板子＝换目录（下面那节的原因）。
- 脚本自己找 SDK（`$PICO_SDK_PATH` → `~/.pico-sdk` → `~/pico/pico-sdk` → `/opt/pico-sdk`），
  找不到就直接报错，不做猜测。

## 构建目录：**必须用 RP2350 的那个**

| 目录 | `PICO_BOARD` | `PICO_PLATFORM` | 说明 |
| --- | --- | --- | --- |
| `build/` | `pico` | `rp2040` | ⚠️ **RP2040**，默认会选错 |
| `build-pico2/` | `pico2` | `rp2350-arm-s` | ✅ **RP2350**，本项目当前使用 |

**这是最容易踩的坑**：项目默认按 RP2040 配置，直接 `mkdir build && cmake ..` 出来的固件
烧到 Pico 2 上跑不起来。

```bash
cd build-pico2
cmake .. -DPICO_BOARD=pico2          # 首次配置
cmake --build . -j8
```

（手工命令与 `./build.sh -b pico2` 完全等价 —— 后者就是这两条 + 前面选好的 `-DPUD_CONFIG`。）

产物：

| 文件 | 用途 |
| --- | --- |
| `pico-usb-display.elf` | gdb/OpenOCD 烧录与调试（带符号） |
| `pico-usb-display.uf2` | 拖到 USB 大容量盘（BOOTSEL 模式）烧录 |

`PICO_PLATFORM=rp2350-arm-s` 表示用 **Cortex-M33（安全态）**，而不是 Hazard3 RISC-V。

CMake 还提供了 `flash` 目标（`CMakeLists.txt` 里按板子选 `target/rp2040.cfg` 或
`target/rp2350.cfg`），它要求 **openocd 与构建在同一台机器上** —— 在原生 Linux 开发机上
（openocd 就在本机）可以直接 `cmake --build build-pico2 --target flash`；
只有 OpenOCD 跑在 Windows 宿主机那套接法下，才要改用下面的 gdb 方式。

## 编辑器 / clangd

`.clang-format`、`.clangd`、`.vscode/`、`.editorconfig` 都在仓库根，开箱可用 ——
**前提是先 configure 过一次**：`compile_commands.json` 由 CMake 写进构建目录
（`CMakeLists.txt` 已设 `CMAKE_EXPORT_COMPILE_COMMANDS ON`）。

- 编辑器**不写死任何构建目录**：`.clangd` 与 `.vscode/settings.json` 都不指定路径，
  clangd 靠自己的发现规则去找 `compile_commands.json`（先搜源文件所在的树，再搜
  `build/`）。`build-pico2/` 不在这个默认范围里，所以 `CMakeLists.txt` 在每次
  **configure** 时把仓库根的 `compile_commands.json` **符号链接**指向该构建目录
  （只替换链接，绝不动真实文件 ✓）：

  `build-pico2/`（RP2350）和 `build/`（RP2040）谁最后 configure，clangd 就用谁的编译
  参数 —— 换板子重新 configure 那个目录就行，配置本身一个字不用改；只想让编辑器换过去、
  不整体重编，`cmake -S . -B build-pico2 -DPICO_BOARD=pico2` 就够了。
  链接是生成物（`.gitignore` 已忽略）。注意 clangd 自己也会把 `build/` 当候选：链接不在时
  它会**悄悄**用 RP2040 的参数，那比没有索引更容易误导；删掉 `build-pico2/` 之后重新
  configure 即恢复（曾经因为直接删掉它，clangd 完全没有索引）。
- 编译命令调用的是 `arm-none-eabi-gcc`，clangd 得向它索取内建头文件路径，否则会
  报找不到 `stdint.h`：VS Code 已传 `--query-driver=**/arm-none-eabi-*`；命令行单跑用
  `clangd --check=src/cherryusb/usb.c --query-driver=/usr/bin/arm-none-eabi-*`
  （结尾的 `All checks completed, 0 errors` 就是通过）。
- 风格是**内核风格**（tab + 8 宽，`.clang-format` 取自内核，唯一偏离是
  `UseTab: ForIndentation`），保存即格式化。**vendored 与生成的代码不吃这套**：
  `src/decoders/{qoi,rle,jpegdec,tjpgd}/` 各放一份 `DisableFormat: true` 的
  `.clang-format`；`include/bootlogo.h`、`tools/stb_*.h`、`FreeRTOSConfig.h`、
  `src/cherryusb/usb_config.h` 里写了 `// clang-format off` —— 后两个是为了保住
  `#define` 选项的列对齐，前两个分别是生成文件和 vendored 文件。

## 烧录（`./build.sh flash`）

`lunch` 里选好烧录方式（写进 `.pud-config` 的 `FLASH=`），`./build.sh flash` 就照它烧：

```bash
./build.sh lunch                   # 板子 / 面板配置 / 烧录方式
./build.sh flash                   # 按选中的方式烧 build*/ 里的产物
./build.sh flash -m blackmagic     # 这次换一种方式
./build.sh flash -n                # 只打印命令行，什么都不执行
./build.sh flash -t /dev/ttyACM1   # 换 GDB server / 探针串口
```

| `FLASH=` | 方式 | 命令（脚本里就是这条） | 状态 |
| --- | --- | --- | --- |
| `picotool` | 板子在 BOOTSEL（应用态也能自己送进去，见下），走 USB，本机烧 `build*/*.uf2` | `picotool load -v -x <uf2>` | ✅ **实测**（2026-09-27，RP2040）：`Loading into Flash` 0→100%、`Verifying Flash` 0→100%、`OK`、`The device was rebooted to start the application.`，随后枚举回 `2e8a:0001`；**应用态直接烧**（`-m picotool --reboot`）也实测通过 |
| `openocd` | 本机 openocd + CMSIS-DAP 直连（Debug Probe） | `openocd -f interface/cmsis-dap.cfg -c "adapter speed 10000" -f target/rp2040.cfg -c "program <elf> verify reset exit"` | ✅ **实测**（2026-09-27，openocd 0.12.0+dev / RP2040 rev 2 / 4 MB flash）：`** Verified OK **`，exit 0 |
| `gdb` | gdb 连一个已经在跑的 GDB server（OpenOCD，默认 `localhost:3333`） | 下面那段实测命令 | ✅ **实测**（2026-09-26 在 Windows 宿主 OpenOCD 上；2026-09-27 又用本机 Debug Probe + 本机 openocd 复测：`load size 113300`、`73 KB/sec`） |
| `blackmagic` | 黑魔法探针：gdb 直连它自带的 GDB server（默认 `/dev/ttyACM0`） | `gdb ... -ex "target extended-remote /dev/ttyACM0" -ex "monitor swdp_scan" -ex "attach 1" -ex "load" -ex "compare-sections" -ex "kill"` | **未验证**（手头没有 BMP） |
| `none` | 只构建，自己拖 uf2 | — | 拷贝 uf2 到 `RPI-RP2` 挂载盘这条路实测过（225 KB 约 1.3 s） |

- 目标选择跟着板子走：`pico` → `build/` + `target/rp2040.cfg`，`pico2` → `build-pico2/` +
  `target/rp2350.cfg`（和 CMake 的 `flash` 目标一致）。
- gdb 用哪个二进制：依次找 `gdb-multiarch` → `arm-none-eabi-gdb` → `gdb`。**架构支持是 gdb
  自己带的**，2026-09-27 实测本机只有 `/usr/bin/gdb`（没有 multiarch/arm-none-eabi 那份）
  也能烧，所以缺 `gdb-multiarch` 不等于这条路走不了。
- 工具不在本机时：`-n` 照样把命令行打出来（缺工具只是提示一行），**真烧**就会直接报错，
  路径可以用 `PUD_GDB` / `PUD_PICOTOOL` / `PUD_OPENOCD` 覆盖。
- `picotool` 这一路要求板子**已经在 BOOTSEL**：按住 BOOTSEL 插拔，或者用
  `./build.sh flash --reboot` 让脚本自己把它送进去（实测 2026-09-27，从应用态一路烧完）：
  - 先试 picotool 自己的 `picotool reboot -f -u` —— **对本项目固件无效**：板子在跑应用时只会回
    `No accessible RP-series devices in BOOTSEL mode were found.`，因为那需要设备端实现 picoboot
    的复位接口（本固件没有）。
  - 于是改走 bootrom（RP2040 + 接了 CMSIS-DAP 探针时）：调 ROM 函数 `UB` = `reset_usb_boot(0, 0)`。
    它**在调用里就复位芯片**，所以 openocd 必定报
    `Failed to call ROM function batch / ROM API call failed` —— 那是正常的，等 2 s
    `lsusb` 里就会出现 `2e8a:0003`。RP2350 没有这条（重启 API 是 `RB`，标志位没验证过）。
  ```bash
  # openocd 的 rom_api_call 来自上游那个 "allow arbitrary ROM API call from Tcl" 补丁
  openocd -f interface/cmsis-dap.cfg -c "adapter speed 10000" -f target/rp2040.cfg \
      -c "init" -c "targets rp2040.core0" -c "halt" -c "flash probe 0" \
      -c "rp2xxx rom_api_call UB 0 0" -c "shutdown"
  ```
- BOOTSEL 下板子就是一块 **USB 大容量盘**：不想装 picotool 时，把 `build*/*.uf2` 拷进
  自动挂载的 `RPI-RP2`（或 `RP2350`）卷、等它卸载就烧完了 —— 和拖拽烧录是同一件事，
  实测 225 KB 约 1.3 s。`FLASH=none` 就是这个意思。
- `gdb` 与 `blackmagic` 的区别只在连谁：前者连别人跑着的 OpenOCD，后者连探针自己的
  GDB server，并且多一段 `monitor swdp_scan` / `attach 1`（BMP 的多点扫描）。

### 手工：Windows 宿主机 OpenOCD + gdb（实测）

先在 **Windows 宿主机**启动 OpenOCD（WSL 里看不到调试器和 USB 设备）：

```bash
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "adapter speed 10000"
```

然后在 WSL/开发机上用 `gdb-multiarch` 连 `localhost:3333` 烧录：

```bash
gdb-multiarch -q -nh \
  -ex "set pagination off" -ex "set confirm off" \
  -ex "file pico-usb-display.elf" \
  -ex "target extended-remote localhost:3333" \
  -ex "monitor reset halt" \
  -ex "load" \
  -ex "monitor reset run" \
  -ex "detach" -ex "quit"
```

> `-q -nh` 是必要的：不加会去读 `~/.gdbinit`，某些环境下（如装了 gef）会报错中断脚本。
> 项目里的 `.gdbinit` 只有一行 `target extended-remote :3333`。

**原生 Linux 上实测（2026-09-26）**：上面这条原样可用 —— 各段 `.text/.rodata/.data/
.noncacheable` 全部写入，`load size 154376`（= 本次构建的 `text 55936 + data 98440`）、
`Transfer rate 63 KB/sec`；烧完主机看到干净的重新枚举（设备号 070 → 071），
`PUD_CMD_GET_CAPS` 读回的 caps 与烧录前**逐字段一致**。

成功输出形如：

```
Loading section .text, size 0xce40 lma 0x10000000
Loading section .noncacheable, size 0x204fc lma 0x10015890
...
Start address 0x1000014c, load size 220576
```

## 配置项

### `CMakeLists.txt`

| 配置 | 当前值 | 说明 |
| --- | --- | --- |
| 显示配置 include | `pico_dm_qd3503728.cmake` | 决定屏型号/分辨率/引脚 |
| `OVERCLOCK_ENABLED` | `1` | 板配置 profile 1：RP2350 225 MHz / QSPI 75 MHz / 1.10V，见 [architecture.md](architecture.md) |
| `PIO_USE_DMA` | `1` | I8080 走 PIO + DMA（早期 DREQ 卡死已复测通过） |
| `DECODER_TYPE` | `3` | 3 = QOI |

（不写 `CMakeLists.txt` 的行号了 —— 加一行就漂，已经漂过一次。）

### 显示配置（`configs/*.cmake`）

仓库自己的 `configs/` 下每个面板一个 `.cmake`（20 个），由 `-DPUD_CONFIG=<名字>`
选中（`./build.sh lunch` 写进 `.pud-config`，也可以 `./build.sh -c <名字>` 临时指定）。
常用：`pico_dm_qd3503728`（当前面板）、`pico_dm_yt350s006`、`generic-ili9341`、
`generic-st7789v`；其余见 `./build.sh configs`。

`lib/pico-display-lib/configs/` 是同一批文件的**上游来源**，构建不再 include 它 ——
改本机面板参数改仓库这份即可，不用动子模块，也不会在子模块更新时被覆盖。

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

**改了分辨率要同步检查**：
1. 驱动侧的分带尺寸（`pud->max_band_pixels`，由 `PUD_CMD_GET_CAPS` 从设备得到）与 `tx_buf` 大小
2. 固件的 QOI 缓冲尺寸（`qoi_buf_a/b[480 * QOI_BUF_ROWS]` 里的 480 是硬编码上限）
3. `include/bootlogo.h` 里的 logo 数据是按分辨率编好的

## 增量构建的注意点

- `DECODER_TYPE` 改了之后要重新 `cmake ..`（它是 `target_compile_definitions` 传入的）。
- `include/bootlogo.h` 是按解码器类型分支的大数组，改 `DECODER_TYPE` 会切换整个数组 ——
  文件很大（4500+ 行），编辑器操作要小心：
  **不要用会截断文件的命令**（曾经用 `sed` 处理这个文件时因参数列表过长导致文件被清空，
  最后靠 `git checkout -- include/bootlogo.h` 恢复）。
- 构建产物（`build*/`）在 `.gitignore` 里，不要提交。
