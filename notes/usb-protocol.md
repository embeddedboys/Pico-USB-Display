# USB 厂商协议（设备侧）

> **字段定义的权威来源是** `PUD-kernel-drivers/notes/usb-protocol.md`。
> 本文只讲**设备侧怎么处理**，与那份文档配对阅读。

## 端点在固件里的落点

| 端点 | 描述符 | 接收缓冲 | 回调 |
| --- | --- | --- | --- |
| EP1 OUT (bulk) | `USB_BULK_EP_MPS_FS` | `ep1_read_buffer[EP1_RD_BUF_SIZE]` = **`PUD_MAX_TRANSFER`：RP2350 65536 / RP2040 32768 字节** | `usbd_vendor_ep1_bulk_out()` |
| EP2 IN (bulk) | `USB_BULK_EP_MPS_FS` | `ep2_write_buffer[EP2_WR_BUF_SIZE]` = 128 | `usbd_vendor_ep2_bulk_in()`（空实现） |
| EP4 IN (int, 64B, bInterval 33) | — | `ep4_write_buffer[EP4_WR_BUF_SIZE]` = 128 | `usbd_vendor_ep4_int_in()`（空实现） |

`ep1_read_buffer` 等用 `USB_NOCACHE_RAM_SECTION` + `USB_MEM_ALIGNX` 声明，
保证不被 cache 影响、且满足 USB 控制器的对齐要求。

`EP3` 在 `usbd_vendor.h` 里有定义（`REQ_EP3_OUT` / `EP3_OUT_ADDR`），
但**没有写进配置描述符**，主机看不到它。

## 请求分发

所有厂商请求都在 `vendor_request_handler()` 里分发（`src/cherryusb/usbd_vendor.c`）：

```c
switch (setup->bRequest) {
case REQ_EP1_OUT:  /* 图像帧：窗口协商 */
case REQ_EP2_IN:   /* 查询：命令 + 长度 */
case REQ_EP4_IN:   /* 触摸 */
default:           return -1;
}
```

## EP1 图像帧的处理（含流控）

```c
case REQ_EP1_OUT:
    req_ep1_out = (struct req_ep1_out *)*data;

    /* 先校验：主机声明的 size 会被原样当成 EP1 读长度用进 ep1_read_buffer，
     * 超限就返回 -1 让 CherryUSB stall EP0（主机侧立刻拿到错误，而不是
     * 缓冲越界、或者一次永远不被武装的批量传输挂到超时）。 */
    if (!usbd_vendor_ep1_size_ok(req_ep1_out->size))
        return -1;

    decoder_set_window(req_ep1_out->xs, req_ep1_out->ys,
                       req_ep1_out->xe, req_ep1_out->ye);

    /* Flow control: only accept the frame once a decoder slot is free. */
    if (!decoder_slot_free()) {
        usbd_vendor_ep1_defer(req_ep1_out->size);
        return 0;                     /* 故意不武装 EP1 */
    }
    return usbd_ep_start_read(busid, EP1_OUT_ADDR, ep1_read_buffer,
                              req_ep1_out->size);
```

数据到达后：

```c
void usbd_vendor_ep1_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    if (!nbytes) return;
    /* 不在中断里解码！见 pitfalls.md */
    decoder_submit_frame(decoder_xs, decoder_ys, decoder_xe, decoder_ye,
                         ep1_read_buffer, nbytes);
}
```

**注意 `xe`/`ye` 是闭区间**：固件算宽度用 `xe - xs + 1`。

### 流控为什么这样设计

固件只有 2 个解码帧槽（`DECODER_FRAME_SLOTS`）。主机以数百帧/秒推送局部刷新时，
解码任务容易跟不上。最初的实现是"槽满就丢帧" —— 结果是**残影**：
丢掉的那帧里有某个区域的最新内容，随后又被一帧旧内容覆盖回去。

改成**背压**：槽满时不武装 EP1。因为主机的批量传输紧跟在其控制请求之后，
端点没武装，主机的 bulk 写入自然阻塞等待 —— 协议不需要任何改动，
主机侧（`usb_sg_wait()`）也无需感知。

代价与前提：
- 主机的 `pud_flush()` 有 3 秒超时看门狗；解码是毫秒级，正常不会触发。
- 主机在 bulk 传输期间不会发出下一个控制请求，所以 `decoder_set_window()` 设置的
  全局窗口在被延迟处理期间不会被覆盖 —— **这是该设计成立的关键前提**。

### 延迟武装的补发

`usbd_vendor_ep1_arm/defer/tick` 三个函数（`src/cherryusb/usb.c`）：

```c
static volatile uint32_t s_ep1_pending_size;

void usbd_vendor_ep1_defer(uint32_t size) { s_ep1_pending_size = size; }

/* 由解码任务在释放帧槽后调用 */
void usbd_vendor_ep1_tick(void)
{
    uint32_t size = s_ep1_pending_size;
    if (size) {
        s_ep1_pending_size = 0;
        usbd_vendor_ep1_arm(size);
    }
}
```

调用点在 `decoder_task()` 释放槽位之后。由于主机被阻塞、不可能并发下发新的控制请求，
这个标志位不存在竞态（但仍声明为 `volatile` 以防编译器优化掉读）。

## EP2 查询的处理

```c
case REQ_EP2_IN:
    req_ep2_in = (struct req_ep2_in *)*data;    /* { u16 cmd; u16 size; } */
    /* fsm 返回真正产生的字节数：按主机请求的长度发会把 ep2_write_buffer
     * 里的陈旧内容一起漏出去（实测 cmd 0x7f 曾把上一次的 caps 报告发回）。 */
    return usbd_ep_start_write(busid, EP2_IN_ADDR, ep2_write_buffer,
                               usbd_vendor_ep2_bulk_in_fsm(req_ep2_in->cmd,
                                                           req_ep2_in->size));
```

`usbd_vendor_ep2_bulk_in_fsm()` 实现：

| cmd | 名称 | 动作 |
| --- | --- | --- |
| `0x01` | `PUD_CMD_GET_SN` | `pud_get_ro_sn(ep2_write_buffer, len)` 填 8 字节唯一 ID，返回 `len`（行为不变） |
| `0x02` | `PUD_CMD_GET_CAPS` | 填 `struct pud_caps`（16 B），返回 `min(len, 16)` |
| 其他 | — | 返回 **0**（发零长度包），主机侧读回短包 → 判定"不支持" |

### `PUD_CMD_GET_CAPS`（设备能力）

主机靠这个命令知道自己一次最多能发多少字节——同一个数字决定了固件侧的
`EP1_RD_BUF_SIZE` 和帧槽大小（`PUD_MAX_TRANSFER`），而它是**按板子**定的
（RP2040 只有 256 KB SRAM，见 [pitfalls.md](pitfalls.md) 的 3.4）。这样一份主机
代码就能同时服务两种板子，不必按板重编驱动。

```c
struct pud_caps {
    u32 magic;          /* PUD_CAPS_MAGIC = 0x43445550 ("PUDC") */
    u32 proto_ver;      /* PUD_PROTO_VER = 1 */
    u32 frame_max;      /* 单次 EP1 传输上限：RP2350 65536，RP2040 32768 */
    u32 decoder_type;   /* 0 tjpgd, 1 JPEGDEC, 2 LZ4, 3 QOI */
};
```

**必须校验 `magic`**：没有这个命令的固件会用 EP2 缓冲里的残留内容应答
（实测：`cmd=0x7f` 曾返回上一次的 caps），所以"发回来 16 字节"不等于"支持该命令"。
驱动与 `scripts/pud_usb.py` 都在 magic 不匹配时退回本机默认值（65535 B / 21839 px），
并打印一条告警。RP2350 上 `frame_max=65536` 经 `min(65535, …)` 后与改前的编译期常量
**完全一致**，因此分带行为零变化。

## EP4 触摸的处理

**设备主动推送**，主机常挂一个 interrupt IN 的 URB（输入端点的正常形态）：

```c
/* 触摸任务每 tp.polling_period(33) ms 轮询一次控制器 */
pressed = indev_is_pressed();
if (pressed || 刚刚松开) {
    report = ...;                      /* 见下 */
    usbd_vendor_ep4_submit(&report);   /* 空闲则武装 EP4，在途则覆盖 */
}
```

`REQ_EP4_IN`（0x05）**保留**：它把**当前**这一帧发给主机，给"轮询式"主机用
（旧驱动就是这么写的）。两种模型共存，互不干扰。

### 上报格式（8 字节，逐字节定义）

| 字节 | 含义 |
| --- | --- |
| 0 | flags，bit0 = 按下 |
| 1–2 | x >> 8、x & 0xff（大端） |
| 3–4 | y >> 8、y & 0xff |
| 5 | sequence（回绕；跳变说明主机漏采/被合并） |
| 6 | version = `PUD_TOUCH_VERSION`(1)，主机据此判断这版固件真的支持触摸 |
| 7 | 保留，0 |

前 5 个字节与"旧驱动已经在解析"的布局**完全一致**（`buf[0]` 按下、`buf[1..2]` x、
`buf[3..4]` y），所以驱动的字段偏移不用改；多出来的是 sequence/version。

**同一时刻只允许一帧在途**，期间到来的新样本**覆盖**旧的（只保留最新状态，
排队只会加延迟）；传输完成回调里若发现有更新的样本会立即补发。

**坐标是显示坐标系下的面板坐标**（0..479 / 0..319），由库按 `TFT_ROTATION` 变换，
与面板驱动用的是同一个数字；固件侧再钳一次，保证上线值不越界。触摸控制器本身的轴序/
反向/偏移**不再由各驱动处理**，见 `AGENTS.md` 的"架构不变量"。

### 已验证（2026-09，SWD 强制控制器原始值）

用 gdb 把 `ft6236_read_x/read_y` 的返回值钉成 `raw_x=100, raw_y=200`（`is_pressed`
强制为 1），从主机读 EP4：

| `TFT_ROTATION` | 上报 | 对应关系 |
| --- | --- | --- |
| 1 | (200, 219) | x = raw_y，y = 319 - raw_x |
| 3 | (279, 100) | x = 479 - raw_y，y = raw_x |

两者正好相差 180°，说明变换确实由旋转推导。顺带验证：按下期间每 33 ms 一帧、
`version=1`、空闲时主机读会超时（设备不发声）。

**真实手指（2026-09，`touch_draw.py --mode grid --rate 20`）**：29 次触摸、193 个报告、
29 个 release（每次触摸都有收尾，没有卡在按下）：

- 四角落点 (0,0)、(475,0)、(475,319)、(0,314) —— 象限正确，**无轴交换/反向**（电容屏+
  贴合偏移，摸不到 479/319 是正常的，固件端已钳位，实测无越界值）；
- 横拖只变 x（`y=195` 恒定，x 48..371）、竖拖只变 y（`x=234` 恒定，y 42..298）；
- 全程 sequence 只跳了 **2 次**（即 2 个样本被合并：设备在上一帧还在途时用最新样本覆盖，
  这是设计行为）；面板更新限速 20 Hz 时，**板子 10 分钟没有挂**。

> 空闲时**不发**任何帧，所以主机不要指望"心跳"。驱动侧如果担心丢失"松手"那一帧，
> 自己加超时判定（见 `notes/todo.md`）。

## 已经修掉的协议实现问题

- **EP2 访问器的长度原先直接取主机给的值**（`struct req_ep2_in.size`），而 setter 往
  定长字段里 `memcpy` —— 一次 8 字节查询就会写坏 `cmd` 之后的 `g_pud_data.disp`
  （实测每查一次 `disp` 就成垃圾；长度再大就会踩穿 RAM）。现在四个访问器宏都把长度钳到
  字段大小，FSM 也只用 `sizeof(cmd)` 记录命令。
- **FT6236 的 XH/YH 高 4 位是事件标志和 touch id**，坐标只占低 4 位；库原先用 `& 0x1f`
  读 XH（把 touch id 留下了）、YH 完全没掩码，于是拖拽中途会出现 +4096 的跳变
  （实测 193 → 4136 并持续 36 个样本）。现在两端都 `& 0x0f`。

## 协议层的已知不一致（两侧都要知道）

1. **主机用 16 字节 wLength 传 12 字节的有效结构**（`struct req_ep1_out`）。
   固件只读前 12 字节，所以现在能工作。
2. **`size` 可能比真实压缩长度大 1**（主机为 RP2350 做了向上取偶）。
   QOI 解码在结束标记处停止，不会消费多余字节。

详见 `PUD-kernel-drivers/notes/usb-protocol.md` 的两节"已知不一致"。

## 改协议时的检查清单

1. `src/cherryusb/usbd_vendor.c` 的结构体与 `vendor_request_handler()`
2. `src/cherryusb/usbd_vendor.h` 的 `REQ_*` / 端点地址 / 缓冲尺寸
3. 驱动 `usb.c` 的对应结构体与 `pud_feed_ctrl_buf()`
4. 两侧 notes 里的协议文档
