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

产物：

| 文件 | 用途 |
| --- | --- |
| `pico-usb-display.elf` | gdb/OpenOCD 烧录与调试（带符号） |
| `pico-usb-display.uf2` | 拖到 USB 大容量盘（BOOTSEL 模式）烧录 |

`PICO_PLATFORM=rp2350-arm-s` 表示用 **Cortex-M33（安全态）**，而不是 Hazard3 RISC-V。

CMake 还提供了 `flash` 目标（`CMakeLists.txt` 第 137 行起，按板子选 `target/rp2040.cfg`
或 `target/rp2350.cfg`），但它要求 **openocd 与构建在同一台机器上**。
本项目 OpenOCD 跑在 Windows 宿主机，因此改用下面的 gdb 方式。

## 编辑器 / clangd

`.clang-format`、`.clangd`、`.vscode/`、`.editorconfig` 都在仓库根，开箱可用 ——
**前提是先构建过一次**：`compile_commands.json` 由 CMake 写进构建目录
（`CMakeLists.txt` 已设 `CMAKE_EXPORT_COMPILE_COMMANDS ON`）。

- `.clangd` 里的 `CompilationDatabase: build-pico2` 指向它，`.vscode/settings.json`
  的 `--compile-commands-dir` 同理；换构建目录（RP2040 的 `build/`）要一起改。
- 编译命令调用的是 `arm-none-eabi-gcc`，clangd 得向它索取内建头文件路径，否则会
  报找不到 `stdint.h`：VS Code 已传 `--query-driver=**/arm-none-eabi-*`；命令行单跑用
  `clangd --check=src/pud.c --query-driver=/usr/bin/arm-none-eabi-*`
  （结尾的 `All checks completed, 0 errors` 就是通过）。
- 风格是**内核风格**（tab + 8 宽，`.clang-format` 取自内核，唯一偏离是
  `UseTab: ForIndentation`），保存即格式化。**vendored 与生成的代码不吃这套**：
  `src/decoders/{qoi,rle,jpegdec,tjpgd}/` 各放一份 `DisableFormat: true` 的
  `.clang-format`；`include/bootlogo.h`、`tools/stb_*.h`、`FreeRTOSConfig.h`、
  `src/cherryusb/usb_config.h` 里写了 `// clang-format off` —— 后两个是为了保住
  `#define` 选项的列对齐，前两个分别是生成文件和 vendored 文件。

## 烧录（CMSIS-DAP / OpenOCD + gdb）

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

### 显示配置（`lib/pico-display-lib/configs/*.cmake`）

切换屏幕时**注释掉一个、启用一个**（`CMakeLists.txt` 第 34-38 行），可选：
`pico_dm_qd3503728`、`pico_dm_yt350s006`、`generic-ili9341`、`generic-st7789v` 等
（`configs/` 下还有 gc9a01 / st7305 / st7576 / st77916 / ssd1306 / ssd1327 / fpc-zh096g1321）。

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
