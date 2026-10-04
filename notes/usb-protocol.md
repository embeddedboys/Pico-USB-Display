# USB 厂商协议（设备侧）

> **字段定义的权威来源是** `PUD-kernel-drivers/notes/usb-protocol.md`。本文只讲**设备侧怎么处理**；
> 两边字段必须语义一致，改协议要同步这一份与那两份。触摸通道见 [touch-ep4.md](touch-ep4.md)，
> reset 接口见 [reset-interface.md](reset-interface.md)。

## TL;DR

- 协议 v2：每次 EP1 传输 = `struct pud_ep1_header`（**12 B**）+ 载荷，矩形与长度随数据走，**没有窗口协商控制请求**（`REQ_EP1_OUT` 已删，发它会被 stall）。
- EP1 读法**两段**：先要一个 max packet（header 必在其中），再按 header 的 `size` 要剩下的字节 —— 传输结束不依赖短包。
- 槽满时**不武装 EP1**（背压）；不可信 header **丢弃并重新武装，不 stall**（stall 会把宿主控制器卡死）。
- 查询走 EP2 IN：`0x01 GET_SN` / `0x02 GET_CAPS`（32 B）/ `0x04 GET_PARAM`（12 B）/ `0x05 GET_QOID`（60 B，实验）；未知 cmd 回**零长度**，不发陈旧内容。
- 运行期参数写走控制 OUT（`REQ_SET_PARAM` 0x06 + `PUD_CMD_SET_PARAM` 0x03），只允许寄存器级操作。

Pico/Pico 2 固件使用 `0x2E8A:0x0001`，ESP32-S3 移植版使用
`0x303A:0x3503`；两者的接口、端点和 EP2/EP1 协议字段相同，主机工具应兼容两组 ID。

## 端点在固件里的落点

| 端点 | 描述符 | 接收/发送缓冲 | 回调 |
| --- | --- | --- | --- |
| EP1 OUT (bulk) | `USB_BULK_EP_MPS_FS` | `ep1_read_buffer[EP1_RD_BUF_SIZE]` = **`PUD_MAX_TRANSFER`：RP2350 65536 / RP2040 32768 字节**（含 12 B header） | `usbd_vendor_ep1_bulk_out()` |
| EP2 IN (bulk) | `USB_BULK_EP_MPS_FS` | `ep2_write_buffer[EP2_WR_BUF_SIZE]` = 128 | `usbd_vendor_ep2_bulk_in()`（空实现） |
| EP4 IN (int, 64B, `bInterval` 8) | — | `ep4_write_buffer[EP4_WR_BUF_SIZE]` = 128 | `usbd_vendor_ep4_int_in()`（上报在途时收到新样本就重新武装） |

`ep1_read_buffer` 等用 `USB_NOCACHE_RAM_SECTION` + `USB_MEM_ALIGNX` 声明，保证不被 cache 影响、
且满足控制器的对齐要求。`EP3` 在 `usbd_vendor.h` 里有定义（`REQ_EP3_OUT` / `EP3_OUT_ADDR`），
但**没有写进配置描述符**，主机看不到它。

## 请求分发

厂商请求都在 `vendor_request_handler()`（`src/cherryusb/usbd_vendor.c`）里分发：

```c
switch (setup->bRequest) {
case REQ_EP2_IN:    /* 查询：命令 + 长度 */
case REQ_SET_PARAM: /* 运行期参数：命令 + 长度 + 参数本体 */
case REQ_EP4_IN:    /* 触摸 */
default:            return -1;   /* 含已废弃的 REQ_EP1_OUT */
}
```

接口 1（reset）的请求**不在这里**：picotool 发的是 class 型请求，CherryUSB 按 `wIndex` 把它交给
那个接口的 `class_interface_handler`，所以 reset 接口有自己的一份 `reset_request_handler()`。

`REQ_EP1_OUT`（0x02）**在协议 v2 里被删掉了**（矩形随 EP1 数据一起走）。老主机发它会拿到 EP0
stall —— 这是**故意**的，静默按旧格式解析只会更糟。

## EP1 图像帧的处理（含流控）

协议 v2 每次 EP1 传输是 **`struct pud_ep1_header`（12 B）+ 载荷**：

```
[ xs | ys | xe | ye | size ][ payload ]
```

读法是**两段**（`src/cherryusb/usb.c`）：先要一个最大包（`EP1_FIRST_READ` = 64 B，header 一定在
里面）；再从 header 的 `size` 算出总长，要剩下的 `12 + size - 已收` 字节。这样**传输的结束不
依赖短包**，代价是每帧多一次重新武装。核心分支：

```c
s_ep1.armed = false;
if (!nbytes) { ep1_finish(false); return; }        /* ZLP：结束一笔空传输 */
s_ep1.got += nbytes; ep1_mark();                   /* 进度：超时计时看这个 */
if (!s_ep1.total) {                                /* 还在读 header 那个包 */
    if (s_ep1.got < PUD_EP1_HEADER_SIZE) {
        if (nbytes < EP1_FIRST_READ) { g_ep1_stat.bad++; ep1_finish(false); }
        else                          ep1_read_more();
        return;
    }
    memcpy(&s_ep1.hdr, ep1_read_buffer, sizeof(s_ep1.hdr));
    s_ep1.total = PUD_EP1_HEADER_SIZE + s_ep1.hdr.size;
    if (s_ep1.total > EP1_RD_BUF_SIZE) { g_ep1_stat.oversize++; ep1_finish(false); return; }
    if (s_ep1.hdr.xs > s_ep1.hdr.xe || s_ep1.hdr.ys > s_ep1.hdr.ye ||
        s_ep1.hdr.xe >= g_pud_data.disp.xres || s_ep1.hdr.ye >= g_pud_data.disp.yres) {
        g_ep1_stat.bad++; ep1_finish(false); return;
    }
}
if (s_ep1.got < s_ep1.total) ep1_read_more();
else if (s_ep1.got != s_ep1.total) g_ep1_stat.bad++, ep1_finish(false);
else ep1_finish(true);                             /* 交给 decoder_submit_frame() */
```

**注意 `xe`/`ye` 是闭区间**：固件算宽度用 `xe - xs + 1`；坐标是整屏绝对坐标。诊断计数器在一个
结构体里（gdb 里 `p g_ep1_stat` 一次读完）：`oversize`（声明长度超出 `ep1_read_buffer`）、
`bad`（没有完整 header、矩形越界，或收到的比声明的多）、`stale`（一笔传输连续
`EP1_TRANSFER_MAX_MS` 没有收到任何字节而被丢掉）。正常主机下三个应一直是 0。计时**按"最后一次
收到字节"**走：主机跑掉就再也没有字节，超时能抓到；而在途的大传输每来一个包就刷新。

**`size` 奇偶都可以**（2026-09 起）：老固件拒绝奇数 `size`，理由是"RP2350 要整字"，但每次读都被
钳到单个 max packet，声明的长度根本到不了控制器。RP2350 实测 4 奇 4 偶、987~43271 B 全部落地
（`got == total`）。主机仍可以向上取偶（驱动和 `pud_usb.py` 都这么做），QOI 解码到结束标记就停，
尾字节不参与解码，两种写法解码结果一致。

### 流控为什么这样设计

帧槽有限，主机以数百帧/秒推送局部刷新时解码任务容易跟不上。最初的实现是"槽满就丢帧" —— 结果是
**残影**：丢掉的那帧里有某个区域的最新内容，随后又被一帧旧内容覆盖回去。改成**背压**：槽满时
不武装 EP1，因为主机的批量传输紧跟在其控制请求之后，端点没武装，主机的 bulk 写入自然阻塞等待
—— 协议不需要任何改动，主机侧也无需感知。代价与前提：

- 主机的 `pud_flush()` 有 3 秒超时看门狗；解码是毫秒级，正常不会触发。
- **v2 里没有"窗口全局变量"这回事了**（矩形随数据走），所以旧设计那个"控制请求期间窗口不会被
  覆盖"的前提也随之消失。
- **提交完要立刻重新武装**（`ep1_finish()` 里就调 `tick()`，而不是等解码任务）：载荷已经
  `memcpy` 进帧槽，`ep1_read_buffer` 立刻可复用；晚一步武装时主机会先写下一帧、第一包被 NAK，
  而全速总线上一次 NAK 要等一帧 —— 实测这一条值 **+17%**（桌面全屏 101 ms → 85 ms）。

### 延迟武装的补发

只有一个入口（`src/cherryusb/usb.c`）：

```c
void usbd_vendor_ep1_tick(void)
{
    if (!usb_is_configured())      /* 枚举完成前武装会把端点搞坏 */
        return;
    if (s_ep1.armed || !decoder_slot_free())
        return;
    s_ep1.armed = true; s_ep1.got = 0; s_ep1.total = 0;
    ep1_mark();
    usbd_ep_start_read(0, EP1_OUT_ADDR, ep1_read_buffer, EP1_FIRST_READ);
}
```

`USBD_EVENT_CONFIGURED` 时**先 `usbd_vendor_ep1_reset()` 再 `tick()`**；`USBD_EVENT_RESET` /
`USBD_EVENT_DISCONNECTED` 时 `reset()`。ISR 与解码任务都会碰这份状态，所以它整体 `volatile`
（`g_ep1_stat` 同理，只给调试器看）。

### 失效与自愈

EP1 是 bulk OUT，**设备侧必须挂着一个读**才会收数据；`s_ep1.armed` 只是软件认为"挂着"，硬件里
那个读可能早就没了。已知这几种情况会让它**永久 NAK**（主机侧表现为每次 3 s 后超时、
`flush failed: -110`，屏上再不动，只有插拔才好）：

| 情况 | 症状（真机实测） | 处理 |
| --- | --- | --- |
| 主机在传输中途消失，剩下半笔 | 读一直等永远不来的载荷，下一帧的字节被当成它的载荷 | `poll()` 里连续 `EP1_TRANSFER_MAX_MS`（500 ms）**没有收到任何字节**就丢弃并重新武装（`stale++`） |
| 主机重新配置（SET_CONFIGURATION 会丢掉设备端点里排队的缓冲） | `armed` 卡在 `true`，`tick()` 以为"有读在飞"而不武装，**一个字节都收不到** | `CONFIGURED` 先 `reset()` 再 `tick()` —— **顺序就是修法的全部** |
| 零长度包（ZLP）结束一笔空传输 | 原来直接 `return`，EP1 从此不再武装 | `ep1_finish(false)`，交回 `tick()` |
| 不可信的 header：`12+size` 超限、矩形越界 | 原实现是**故意 stall EP1** | **丢弃这一笔并重新武装**（`oversize++` / `bad++`），不再 stall |

**为什么不再 stall（2026-09 改）**：stall 会让主机的写失败，而主机接下来的动作是 `clear_halt()`
重试 —— 实测这条路会把**宿主控制器**卡死：`usb_sg_wait()`/`usb_sg_cancel()` 不返回、DRM modeset
锁被占住，板子连重启都不干净。驱动靠分带保证正常主机永远不会送出超限的 header，所以这种 header
只可能是流丢了帧同步，丢掉它比 stall 更安全。（注意与 `REQ_EP1_OUT` 那条**控制请求**区分：
那条仍然 stall，因为它是给老主机的明确失败信号。）

`poll()` 只在解码任务空闲等待时运行（`EP1_POLL_PERIOD_MS` = 200 ms），所以它同时也是"整笔传输
超时"的唯一执行点。验收：主机侧 `S/C Bo:6 … 0 <len>`（完成状态 0，而不是 3 s 后被驱动自己的
看门狗取消成 `-104`），设备侧 `submitted/drawn` 增长且 `bad/oversize/stale` 保持 0。

## EP2 查询的处理

```c
case REQ_EP2_IN:
    req_ep2_in = (struct req_ep2_in *)*data;    /* { u16 cmd; u16 size; } */
    /* 返回真正产生的字节数：按主机请求的长度发会把 ep2_write_buffer 里的陈旧内容
     * 一起漏出去（实测 cmd 0x7f 曾把上一次的 caps 报告发回）。 */
    return usbd_ep_start_write(busid, EP2_IN_ADDR, ep2_write_buffer,
                               usbd_vendor_ep2_bulk_in_fsm(req_ep2_in->cmd, req_ep2_in->size));
```

| cmd | 名称 | 动作 |
| --- | --- | --- |
| `0x01` | `PUD_CMD_GET_SN` | `pud_get_ro_sn(ep2_write_buffer, len)` 填 8 字节唯一 ID，返回 `len` |
| `0x02` | `PUD_CMD_GET_CAPS` | 填 `struct pud_caps`（**32 B**），返回 `min(len, sizeof(caps))` |
| `0x04` | `PUD_CMD_GET_PARAM` | 填 `struct pud_param_state`（**12 B**，见 [usb-params.md](usb-params.md)），返回 `min(len, sizeof(st))` |
| `0x05` | `PUD_CMD_GET_QOID` | 填 `struct pud_qoid_state`（**60 B**，实验，见 [qoid.md](qoid.md)），返回 `min(len, sizeof(st))` |
| 其他 | — | 返回 **0**（发零长度包），主机侧读回短包 → 判定"不支持" |

### `PUD_CMD_GET_CAPS`（设备能力 + 面板参数）

主机靠这个命令知道自己一次最多能发多少字节 —— 同一个数字决定了固件侧的 `EP1_RD_BUF_SIZE` 和
帧槽大小（`PUD_MAX_TRANSFER`），而它是**按板子**定的。这样一份主机代码就能同时服务两种板子，
不必按板重编驱动；面板参数（分辨率、旋转、bpp、总线时钟、触摸轮询周期）也走这里，主机**不再
写死 480×320**。

```c
struct pud_caps {
    u32 magic;          /* PUD_CAPS_MAGIC = 0x43445550 ("PUDC") */
    u32 proto_ver;      /* PUD_PROTO_VER = 2；不匹配主机要大声报错 */
    u32 frame_max;      /* 单次 EP1 传输上限（**含 12 字节 header**）：RP2350 65536 / RP2040 32768 */
    u32 decoder_type;   /* 0 tjpgd, 1 JPEGDEC, 2 LZ4, 3 QOI, 4 RLE,
                           5 QOI+deflate（实验）, 6 QOI+deflate+跨帧字典（实验） */
    /* 以下为后来追加：前 16 字节布局没动，只读 16 字节的老主机仍拿得到合法答复 */
    u16 xres;           /* 面板在它被驱动的坐标系下的尺寸 */
    u16 yres;
    u16 pixelclock_khz; /* 驱动面板的总线时钟 */
    u8  rotation;       /* 固件应用的 TFT_ROTATION */
    u8  bpp;            /* 16 */
    u8  intf_type;
    u8  tp_polling_period; /* 触摸轮询周期，ms；没有触摸驱动时为 0 */
    u16 width_mm;       /* 面板有效区，主机用来算输入设备的分辨率（0=未知） */
    u16 height_mm;
    u16 flags;          /* PUD_CAPS_*：这个固件到底有什么 */
};
#define PUD_CAPS_SIZE 32    /* _Static_assert 在 include/pud.h 里 */
#define PUD_CAPS_TOUCH 0x0001  /* 触摸是可选的：多数板级配置 INDEV_DRV_NOT_USED=1 */
```

**必须校验 `magic`**：没有这个命令的固件会用 EP2 缓冲里的残留内容应答（实测 `cmd=0x7f` 曾返回
上一次的 caps），所以"发回来 16 字节"不等于"支持该命令"。驱动与 `tools/pud_usb.py` 都在 magic
不匹配时退回本机默认值（65535 B / 21835 px，面板 480×320/旋转 0）并打印告警；只回了 16 字节的
老固件同样按"没有面板参数"处理（驱动日志里会写 `old firmware: no panel parameters`）。
RP2350 上 `frame_max=65536` 经 `min(65535, …)` 后与改前的编译期常量**完全一致**，因此分带行为
零变化。

## 已经修掉的协议实现问题

- **EP2 访问器的长度原先直接取主机给的值**（`struct req_ep2_in.size`），而 setter 往定长字段里
  `memcpy` —— 一次 8 字节查询就会写坏 `cmd` 之后的 `g_pud_data.disp`（实测每查一次 `disp` 就成
  垃圾；长度再大就会踩穿 RAM）。现在四个访问器宏都把长度钳到字段大小，FSM 也只用 `sizeof(cmd)`
  记录命令。
- **FT6236 的 XH/YH 高 4 位是事件标志和 touch id**，坐标只占低 4 位；库原先用 `& 0x1f` 读 XH
  （把 touch id 留下了）、YH 完全没掩码，于是拖拽中途会出现 +4096 的跳变（实测 193 → 4136 并
  持续 36 个样本）。现在两端都 `& 0x0f`。

## 协议层的已知不一致（两侧都要知道）

1. **主机用 16 字节 wLength 传 12 字节的有效结构**（`struct req_ep1_out`）。固件只读前 12 字节，
   所以现在能工作。
2. **`size` 可能比真实压缩长度大 1**（主机可以选择向上取偶；设备奇偶都接受）。QOI 解码在结束
   标记处停止，不会消费多余字节。

> `decoder_type=5/6` 与 `PUD_CMD_GET_QOID` 是**设备侧在研的实验字段**（2026-10 起两侧文档已同步）：
> 驱动仓 `notes/usb-protocol.md`（权威定义）列出了 `0x03`/`0x04`/`0x05` 三条命令与
> `decoder_type=6` 以及 16 B 子头的字段布局。
> **驱动已经会编码 6**（自带 `tinyc.c`，见驱动仓 `notes/encoders.md`），但**尚未在真机上验证**；
> `0x03`–`0x05` 三条命令驱动仍不发送。详见 [qoid.md](qoid.md)。

## 改协议时的检查清单

1. `src/cherryusb/usbd_vendor.c` 的结构体与 `vendor_request_handler()`
2. `src/cherryusb/usbd_vendor.h` 的 `REQ_*` / 端点地址 / 缓冲尺寸
3. 驱动 `usb.c` 的对应结构体与 `pud_feed_ctrl_buf()`
4. 两侧 notes 里的协议文档

## 相关

- 运行期参数通道：[usb-params.md](usb-params.md)
- 触摸通道（EP4）：[touch-ep4.md](touch-ep4.md)
- picoboot 复位接口：[reset-interface.md](reset-interface.md)
- 帧槽与流控的实现：[frame-pipeline.md](frame-pipeline.md)
