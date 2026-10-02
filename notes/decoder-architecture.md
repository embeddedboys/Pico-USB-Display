# 解码器抽象与 JPEG 实现

> `DECODER_TYPE` 在编译期选定实现，`include/decoder.h` 用一个宏把
> `decoder_drawimg(xs, ys, xe, ye, data, size)` 分发到对应库；所有实现的签名与坐标系一致。

## TL;DR

- 各实现的签名相同：坐标为**整屏绝对坐标**，`xe`/`ye` 为**闭区间**（宽度 = `xe - xs + 1`）。
- `decoder_names[]` 必须覆盖每一个 `DECODER_TYPE`（现为 7 项，带 `_Static_assert`）；曾漏 `"QOI"` 导致 `DECODER_TYPE=3` 越界读。
- **JPEG 两条路都只用于整屏**（`x = y = 0`）：JPEGDEC 在 `x != 0` 时不仅裁切错位，还会把 `decoder_task` 卡死在显示路径上；局刷一律用 QOI（要正确的 JPEG 局刷则用 tjpgd）。
- `DECODER_TYPE` 是协议字段：编号由 `PUD_CMD_GET_CAPS` 上报，**不要重排**。

## 分发

```c
/* include/decoder.h，按 DECODER_TYPE 展开到 qoi_drawimg / rle_drawimg / lz4_drawimg / ... */
#define decoder_drawimg(xs, ys, xe, ye, b, l) qoi_drawimg(xs, ys, xe, ye, b, l)
```

`DECODER_TYPE=6` 额外需要"这条带落在哪个帧槽"，走另一个宏
`decoder_drawimg_slot(..., slot, serial)`；其它类型忽略后两个参数，所以它仍是宏而不是函数。

```c
/* src/decoders/decoder.c；索引即 DECODER_TYPE */
static char *decoder_names[] = { "tjpgd", "JPEGDEC", "LZ4", "QOI", "RLE",
#if PUD_INFLATE == 2
                                 "QOI+deflate (libdeflate)"
#else
                                 "QOI+deflate (tinfl)"
#endif
                                 , "QOI+deflate+dict" };
_Static_assert(DECODER_TYPE < sizeof(decoder_names) / sizeof(decoder_names[0]),
               "decoder_names[] has to cover every DECODER_TYPE");
```

> **坑**：这个数组曾漏掉 `"QOI"`，而 `DECODER_TYPE=3` 会越界读。加解码器别忘了同步它；
> `_Static_assert` 是后来补的兜底。开机串口的 `Decoder type: QOI` 就来自这里，
> 乱了说明数组有问题（见 [debugging.md](debugging.md) 的串口日志）。

## 两种 JPEG 实现（2026-09 实测）

| | tjpgd（0） | JPEGDEC（1） |
| --- | --- | --- |
| 来源 | ChaN TJpgDec R0.03 + Bodmer 的 `swap`，vendored 在 `src/decoders/tjpgd/` | `src/decoders/jpegdec/` |
| 480×320 4:4:4 全屏（当时那张 `bootlogo.jpg`） | 114.6 ms | **76.4 ms** |
| 480×320 4:2:0 全屏（Pillow 重存） | 175.3 ms | **66.1 ms** |
| 局部刷新（`x != 0`，64×64 @ x=208） | ✓ 正确 | ✗ **坐标错并卡死显示** |
| 额外 RAM（净） | +2 KB | 0 |
| 解码栈峰值 | 600 B | 632 B |

两者的语义都是**主机把子图裁好、JPEG 自带尺寸、固件按 `(xs, ys)` 贴图**，`xe`/`ye` 不参与。

**为什么两种都留着**：JPEGDEC 在 `x != 0` 时 `iWidthUsed` 会算出负值，`draw_mcus` 于是把
`xe = x + iWidth - 1` 填成**子图内坐标**（实测 `xs=208 → xe=63`），`len` 变成巨大的无符号数；
一次这样的 flush 就足以让 `decoder_task` 卡在 `tft_video_flush` 里，帧槽永不释放、EP1 永不
重新武装（实测 `submitted/drawn = 2/0`、`s_ep1_pending_size` 一直挂着），主机只能超时。
**规范：JPEG 两条路都只用于整屏；局刷一律走 QOI。**

`draw_mcus` 里必须用 `pDraw->iWidthUsed` 而不是 `pDraw->iWidth`（后者是整图宽度：

```c
pDraw->iWidth       /* 错误：整图宽度，每行像素会偏移错位 */
pDraw->iWidthUsed   /* 正确：本次实际解码的宽度 */
```

补齐非法坐标只是防呆，**修不好 JPEG 局刷**（见 [todo.md](todo.md) 第 1 条）。

### tjpgd 侧踩过的坑

- 上游按 **8×8 块**回调，逐块 flush 会把地址窗口设 **2400 次/帧**；改成攒满 8 行再 flush
  （`TJPGD_GROUP_ROWS`，缓冲 480×8×2 = 7.5 KB）后每帧 40 次 —— 但实测只快 3~7%
  （118.5 → 111~115 ms），说明瓶颈在解码核心，不在刷屏粒度。
- **MCU 高度随色度采样变化**（4:4:4 → 8 行，4:2:0 → 16 行），一次回调可能跨 8 行组的边界，
  所以拷贝必须**按行遍历、跨组即 flush**；按"一块一次"写会按 16 行写进 8 行缓冲 ——
  480 宽的 4:2:0 图会写穿 7.5 KB。当时开机用的那张 `bootlogo.jpg` 恰好是 4:4:4，只用它测
  发现不了 —— 测这条路径要另找一张 4:2:0 的素材（现在的开机图是 PNG）。
- 净 RAM 只有 +2 KB：9.4 KB workspace（`JD_FASTDECODE 2`）+ 7.5 KB 行缓冲，替换掉了同一
  构建里 QOI 的 15 KB 乒乓缓冲（`qoi_buf_a/b`，靠 `--gc-sections` 丢弃）。

## 相关

- 类型表与各专题入口：[decoders.md](decoders.md)
- `DECODER_TYPE` 的构建开关与默认值：[../AGENTS.md](../AGENTS.md)
