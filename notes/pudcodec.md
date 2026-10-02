# 离线转换器 `tools/pudcodec`

> 主机侧 C 工具：图片/帧序列 ↔ 设备能解的码流；编解码类型运行时用 `--codec` 指定，
> 与 `pud_usb.py` 在无损源上**逐字节一致**。

## TL;DR

- 构建：`cmake -S tools -B tools/build && cmake --build tools/build -j`（`stb` 已 vendor，不需要联网；产物在 `tools/build/`，已 gitignore）。
- 它把上游 `rgb565-rle` / `rgb565-qoi` 那六个小工具合成一个，编解码类型用 `--codec` 选。
- **LZ4 输出的是 band 容器**（`[count][offsets][blocks]`，每 band 一个 block），`--band` 默认取"装得下设备 band 缓冲（43674 B）且能整除高度"的最大行数。
- `tools/mkbootlogo.py` 用它重新生成 `include/bootlogo.h` 的四个分支，落盘前自校验。

## 用法

```bash
pudcodec --codec <qoi|rle|lz4|jpeg> img2s   [options] <image>     # 图片 -> 码流
pudcodec --codec <qoi|rle|lz4|jpeg> s2img   [options] <stream>    # 码流 -> 图片
pudcodec --codec <qoi|rle|lz4|jpeg> video2s [options] <frames...> # 帧序列 -> 容器
```

| 选项 | 说明 |
| --- | --- |
| `--codec` | `qoi` / `rle` / `lz4` / `jpeg`；`auto` 表示从 `.h` 里的 `_CODEC` 标签取 |
| `-o` | 输出路径（默认 `<输入>.<codec>.h/.bin`，`s2img` 默认 `<输入>.png`） |
| `-t` | `img2s`/`video2s`：`h`（C 头，默认）或 `bin`；`s2img`：`png`/`jpg`/`bmp`/`tga` |
| `-n` | C 数组名 / 基名（默认从输出文件名推） |
| `-w` `-h` | `img2s`/`video2s` 是缩放目标；`s2img` 读 `.bin` 时**必须给**（码流里没有尺寸，JPEG 除外） |
| `-q` | JPEG 质量，默认 95 |
| `--band` | LZ4 only：每个 block 的行数（默认取"装得下且能整除高度"的最大值） |
| `--raw` | `video2s` 的输入是拼接好的裸 RGB565 帧 |

编解码对应关系：`jpeg` 覆盖 `DECODER_TYPE` 0 和 1，`lz4` 是 2、`qoi` 是 3、`rle` 是 4。
**LZ4 输出的是 band 容器**：`--band` 不指定时取"装得下设备 band 缓冲（43674 B）且能整除图像
高度"的最大行数，这样 band 高度能从 `block 数 / 图像高度` 推回来，`s2img` 与固件的开机 logo
才能重建。`video2s --codec lz4` 对每一帧都这样分带，容器是**扁平的**（帧优先，一帧内自上而下）。

## 重新生成开机 logo

```bash
cmake -S tools -B tools/build && cmake --build tools/build   # 先有工具
python3 tools/mkbootlogo.py            # 重写 include/bootlogo.h
python3 tools/mkbootlogo.py --check    # 只比对，不改文件
```

资产必须是**无损**的（现在是 `assets/bootlogo.png`）：`test_codec_crosscheck.py` 会拿 Pillow
解出的像素重压一遍再和分支逐字节比，JPEG 源会因两套解码器的 IDCT 舍入不同而失败。脚本只重写
每个分支的字节行（标记、注释、声明都不动），**落盘前先反解回来与工具的产物逐字节比对** ——
这个文件曾被一次批量替换清空过（[../AGENTS.md](../AGENTS.md) 不变量 8）。

## 与固件/脚本的一致性（2026-09 实测）

`tests/test_codec_crosscheck.py` 把这条路径与 `pud_usb.py` 对拍，**不需要设备**：

| 检查 | 结果 |
| --- | --- |
| `img2s --codec qoi/rle` vs `pud_usb.qoi_encode/rle_encode`（PNG 源） | **逐字节相同**（photo/noise/gradient 三份，最大 444298 B） |
| `video2s --raw` 的容器：`[count][offsets[count+1]][data]`、帧数据 | 结构正确，第 0 帧与 Python 编码器逐字节相同 |
| `s2img` 往返 | QOI/RLE/LZ4 都是 153600/153600 像素完全相同 |
| `.h` 的 `_CODEC` / `_WIDTH` / `_HEIGHT` / `_SIZE` / 帧表 | 正确；`--codec auto` 能据此自动解码 |
| JPEG 往返 | 最大 10 LSB、平均 0.20 LSB（有损，属正常） |
| `img2s --codec lz4` 的 band 容器 | 结构正确、每 band 都装得下 43674 B、`s2img` 往返像素精确 |
| **`include/bootlogo.h` 的 QOI / RLE / LZ4 分支** | 用同一张图重压，**逐字节相同**（29652 / 49485 / 17585 B） |

两条**已知的不一致**（用压缩工具时要知道的）：

- **JPEG 源图两条路径不逐字节相同**：`stb_image` 与 Pillow/libjpeg 解 JPEG 的 IDCT 舍入不同，
  同一张图一个出 49485 B、一个出 49494 B（数字出自换 PNG 之前那张 `bootlogo.jpg`）。要比字节
  就用无损源（PNG）或 `--raw` 喂同一份 RGB565；差异 ≤1 LSB，屏上看不出来。
- **LZ4 码流不跨版本逐字节一致**：工具 vendor 的 liblz4 是 1.10.0，板子上 python-lz4 4.4.5
  带的是 1.9.x，同一条 band **8 条里有 3 条**压缩结果不同（都合法）。设备只解压，
  `LZ4_decompress_safe` 与版本无关；两条来源的码流**都在板上验证过像素精确**。要比字节就固定
  同一个 liblz4 版本。**因此 `test_codec_crosscheck.py` 的 `python encoder agrees, band for band`
  一项在 1.9.x 的 wheel 上必然 FAIL**（2026-09-26 实测：8 条里 5 条字节相同、3 条不同，但
  **8 条解出来都与原像素精确一致**）—— 这一项比上面的结论更严，别当成回归。

> RGB565 的打包用**截断**（`r >> 3`）而不是四舍五入，跟 `pud_usb.py` 保持一致 —— 这是两条主机
> 路径能逐字节对拍的前提。

## 相关

- 脚本用法与依赖：[scripts.md](scripts.md)
- 脚本的实测数据：[scripts-measurements.md](scripts-measurements.md)
- LZ4 band 约束：[lz4.md](lz4.md)
