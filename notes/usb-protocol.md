# USB 厂商协议（设备侧）

> **字段定义的权威来源是** `PUD-kernel-drivers/notes/usb-protocol.md`。
> 本文只讲**设备侧怎么处理**，与那份文档配对阅读。

## 端点在固件里的落点

| 端点 | 描述符 | 接收缓冲 | 回调 |
| --- | --- | --- | --- |
| EP1 OUT (bulk) | `USB_BULK_EP_MPS_FS` | `ep1_read_buffer[EP1_RD_BUF_SIZE]` = **`PUD_MAX_TRANSFER`：RP2350 65536 / RP2040 32768 字节**（含 12 字节 EP1 header） | `usbd_vendor_ep1_bulk_out()` |
| EP2 IN (bulk) | `USB_BULK_EP_MPS_FS` | `ep2_write_buffer[EP2_WR_BUF_SIZE]` = 128 | `usbd_vendor_ep2_bulk_in()`（空实现） |
| EP4 IN (int, 64B, bInterval 8) | — | `ep4_write_buffer[EP4_WR_BUF_SIZE]` = 128 | `usbd_vendor_ep4_int_in()`（在上报在途时收到新样本就重新武装） |

`ep1_read_buffer` 等用 `USB_NOCACHE_RAM_SECTION` + `USB_MEM_ALIGNX` 声明，
保证不被 cache 影响、且满足 USB 控制器的对齐要求。

`EP3` 在 `usbd_vendor.h` 里有定义（`REQ_EP3_OUT` / `EP3_OUT_ADDR`），
但**没有写进配置描述符**，主机看不到它。

## picoboot 复位接口（第二个接口，跟图像协议无关）

配置描述符有**两个接口**（`bNumInterfaces = 2`）：

| 接口 | 类/子类/协议 | 端点 | 作用 |
| --- | --- | --- | --- |
| 0 | `0xFF / 0x00 / 0x00` | EP1 OUT、EP2 IN、EP4 IN | 图像 / 查询 / 触摸，就是这份文档讲的协议 |
| 1 | `0xFF / 0x00 / 0x01` | 无 | **picoboot 的 reset 接口**：主机请设备重启 |

接口 1 的类码是 Raspberry Pi 的约定，不是我们定的 —— SDK 的 `pico_stdio_usb` 用的就是
同一个（`src/common/pico_usb_reset_interface_headers/include/pico/usb_reset_interface.h`，
`RESET_INTERFACE_SUBCLASS 0x00` / `RESET_INTERFACE_PROTOCOL 0x01`）：

- `bmRequestType = 0x21`（class 型、接口收方）、`bRequest = 0x01`（`RESET_REQUEST_BOOTSEL`）、
  `wValue` 低 7 位 = 要排除在 BOOTSEL 之外的接口掩码 → `reset_usb_boot(0, wValue & 0x7f)`，
  **不返回**（芯片在调用里就复位了，主机看到的是设备消失）；
- `bRequest = 0x02`（`RESET_REQUEST_FLASH`）→ `watchdog_reboot()`，只重启回应用。

有了它，`picotool` 能在**应用态**把板子直接送进 BOOTSEL（`./build.sh flash -m picotool
--reboot`，2026-09-27 实测：应用态 → BOOTSEL → 烧写 → 回应用，全程不用按钮、不用调试器）。
两个坑：picotool 默认的设备过滤只认 bootrom 与 SDK CDC 的 PID，**必须显式给
`--vid 0x2e8a --pid 0x0001`**（脚本已带上，否则它在扫 reset 接口之前就放弃了）；设备选择
选项还要写在命令自己的选项**前面**（`picotool reboot --vid … --pid … -f -u`）。

它不是 PUD 协议的一部分，但它**不是透明的**：接口 0 的类码、端点、请求全没动，`tools/` 里
那些 pyusb 脚本按设备找、按端点地址收发，完全不受影响；**内核驱动则要显式躲开它** ——
`pud_ids[]` 原本是设备级的 `USB_DEVICE(0x2E8A, 0x0001)`，USB 核会为每个匹配到的接口各调一次
`probe()`，而驱动用的是写死的端点地址（`EP1_OUT_ADDR` 等），拿错接口不会报错，只会再注册一个
显示设备。驱动仓现在用 `USB_DEVICE_AND_INTERFACE_INFO(...)` 只匹配图像接口，并在 `pud_probe()`
入口再挡一道（见驱动仓 `notes/usb-protocol.md` 的枚举表）。

> 实测面：上面两条请求都在 **RP2040** 板上跑过（第二个接口与 `reset_usb_boot` 走的就是
> SDK 的实现）。**RP2350 只编过、没在真机验证**（手头只有 RP2040 板）—— 它走的是 SDK 里
> `reset_usb_boot()` 的另一条实现（`rom_reboot`），两者接口一致，但没实测。

## 请求分发

厂商请求都在 `vendor_request_handler()` 里分发（`src/cherryusb/usbd_vendor.c`）：

```c
switch (setup->bRequest) {
case REQ_EP2_IN:    /* 查询：命令 + 长度 */
case REQ_SET_PARAM: /* 运行期参数：命令 + 长度 + 参数本体（见"运行期参数"） */
case REQ_EP4_IN:    /* 触摸 */
default:            return -1;   /* 含已废弃的 REQ_EP1_OUT，见下 */
}
```

接口 1 的请求**不在这里**：picotool 发的是 class 型请求，CherryUSB 按 `wIndex` 把它交给
那个接口的 `class_interface_handler`（`usbd_core.c` 的 `usbd_class_request_handler()`），
所以 reset 接口有自己的一份 `reset_request_handler()`（`usbd_reset_init_intf()` 注册）。

`REQ_EP1_OUT`（0x02）**在协议 v2 里被删掉了**：矩形随 EP1 的数据一起走。老主机发它
会拿到 EP0 stall —— 这是**故意**的，静默按旧格式解析它的数据只会更糟。

## EP1 图像帧的处理（含流控）

协议 v2 每次 EP1 传输是 **`struct pud_ep1_header`（12 B）+ 载荷**，矩形和长度都在里面：

```
[ xs | ys | xe | ye | size ][ payload ]
```

读法是**两段**（`src/cherryusb/usb.c`）：

1. 先要一个最大包（`EP1_FIRST_READ` = 64 B）——header 一定在里面；
2. 从 header 的 `size` 算出总长，再要剩下的 `12 + size - 已经收到的` 字节。

这样**传输的结束不依赖短包**（长度是算出来的），代价是每帧多一次重新武装。

```c
void usbd_vendor_ep1_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    s_ep1.armed = false;
    if (!nbytes) { ep1_finish(false); return; }   /* ZLP：结束一笔空传输 */
    s_ep1.got += nbytes;
    ep1_mark();                                   /* 进度：超时计时看这个 */

    if (!s_ep1.total) {                       /* 还在读 header 那个包 */
        if (s_ep1.got < PUD_EP1_HEADER_SIZE) {
            if (nbytes < EP1_FIRST_READ) { g_ep1_stat.bad++; ep1_finish(false); }
            else                          ep1_read_more();
            return;
        }
        memcpy(&s_ep1.hdr, ep1_read_buffer, sizeof(s_ep1.hdr));
        s_ep1.total = PUD_EP1_HEADER_SIZE + s_ep1.hdr.size;

        /* 不可信的 header：丢弃这一笔并重新武装，**不 stall**（理由见"失效与自愈"） */
        if (s_ep1.total > EP1_RD_BUF_SIZE) {
            g_ep1_stat.oversize++;
            ep1_finish(false);
            return;
        }
        if (s_ep1.hdr.xs > s_ep1.hdr.xe || s_ep1.hdr.ys > s_ep1.hdr.ye ||
            s_ep1.hdr.xe >= g_pud_data.disp.xres ||
            s_ep1.hdr.ye >= g_pud_data.disp.yres) {
            g_ep1_stat.bad++;
            ep1_finish(false);
            return;
        }
    }
    if (s_ep1.got < s_ep1.total) ep1_read_more();
    else if (s_ep1.got != s_ep1.total) g_ep1_stat.bad++, ep1_finish(false);
    else ep1_finish(true);                    /* 交给 decoder_submit_frame() */
}
```

**注意 `xe`/`ye` 是闭区间**：固件算宽度用 `xe - xs + 1`。坐标是整屏绝对坐标。
诊断计数器都在一个结构体里（gdb 里 `p g_ep1_stat` 一次读完）：`oversize`（声明的长度
超出 `ep1_read_buffer`）、`bad`（没有完整 header、矩形越界，或收到的比
声明的多）、`stale`（一笔传输连续 `EP1_TRANSFER_MAX_MS` 没有收到任何字节而被丢掉）。
正常主机下三个应当一直是 0。
计时**按"最后一次收到字节"**走：主机跑掉就再也没有字节，超时能抓到；而在途的大传输每来
一个包就刷新，不会被从中间砍断。

**`size` 奇偶都可以**（2026-09 起）：老固件拒绝奇数 `size`，理由是"RP2350 要整字"，
但每次读都被钳到单个 max packet（见 port 的 `usbd_ep_start_read`），声明的长度根本到不了
控制器。RP2350 实测 4 奇 4 偶、987~43271 B 全部落地（`got == total`）。主机仍可以
向上取偶（驱动和 `pud_usb.py` 都这么做），QOI 解码到结束标记就停，尾字节不参与解码，
两种写法解码结果一致。

### 流控为什么这样设计

固件只有 2 个解码帧槽（`DECODER_FRAME_SLOTS`）。主机以数百帧/秒推送局部刷新时，
解码任务容易跟不上。最初的实现是"槽满就丢帧" —— 结果是**残影**：
丢掉的那帧里有某个区域的最新内容，随后又被一帧旧内容覆盖回去。

改成**背压**：槽满时不武装 EP1。因为主机的批量传输紧跟在其控制请求之后，
端点没武装，主机的 bulk 写入自然阻塞等待 —— 协议不需要任何改动，
主机侧也无需感知（驱动的同步 `usb_sg_wait()` 与异步 completion 都只是多等一会儿）。

代价与前提：
- 主机的 `pud_flush()` 有 3 秒超时看门狗；解码是毫秒级，正常不会触发。
- **v2 里没有"窗口全局变量"这回事了**（矩形随数据走），所以旧设计那个
  "控制请求期间窗口不会被覆盖"的前提也随之消失。
- **提交完要立刻重新武装**（`ep1_finish()` 里就调 `tick()`，而不是等解码任务）：
  载荷已经 `memcpy` 进帧槽，`ep1_read_buffer` 立刻可复用；晚一步武装的话主机会先写
  下一帧、第一包被 NAK，而全速总线上一次 NAK 要等一帧 —— 实测这一条值 **+17%**
  （桌面全屏 101 ms → 85 ms）。

### 延迟武装的补发

只有一个入口（`src/cherryusb/usb.c`）：

```c
/* 由解码任务在释放帧槽后、配置完成时、以及"提交完一帧"时调用 */
void usbd_vendor_ep1_tick(void)
{
    if (!usb_is_configured())      /* 枚举完成前武装会把端点搞坏 */
        return;
    if (s_ep1.armed || !decoder_slot_free())
        return;

    s_ep1.armed = true;
    s_ep1.got = 0;
    s_ep1.total = 0;
    ep1_mark();
    usbd_ep_start_read(0, EP1_OUT_ADDR, ep1_read_buffer, EP1_FIRST_READ);
}
```

`USBD_EVENT_CONFIGURED` 时**先 `usbd_vendor_ep1_reset()` 再 `tick()`**，
`USBD_EVENT_RESET` / `USBD_EVENT_DISCONNECTED` 时 `reset()`。ISR 与解码任务都会碰这份
状态，所以它整体 `volatile`（`g_ep1_stat` 同理，只给调试器看）。

### 失效与自愈

EP1 是 bulk OUT，**设备侧必须挂着一个读**才会收数据；`s_ep1.armed` 只是软件认为"挂着"，
硬件里那个读可能早就没了。已知这几种情况会让它**永久 NAK**（主机侧表现为每次 3 s 后
超时、`flush failed: -110`，屏上再不动，只有插拔才好）：

| 情况 | 症状（真机实测） | 处理 |
| --- | --- | --- |
| 主机在传输中途消失，剩下半笔 | 读一直等永远不来的载荷，下一帧的字节被当成它的载荷 | `poll()` 里连续 `EP1_TRANSFER_MAX_MS`（500 ms）**没有收到任何字节**就丢弃并重新武装（`stale++`） |
| 主机重新配置（SET_CONFIGURATION 会丢掉设备端点里排队的缓冲） | `armed` 卡在 `true`，`tick()` 以为"有读在飞"而不武装，于是**一个字节都收不到**（实测 `got` 恒 0、所有计数器为 0） | `CONFIGURED` 先 `reset()` 再 `tick()` —— **顺序就是修法的全部** |
| 零长度包（ZLP）结束一笔空传输 | 原来直接 `return`，EP1 从此不再武装 | `ep1_finish(false)`，交回 `tick()` |
| 不可信的 header：`12+size` 超限、矩形越界 | 原实现是**故意 stall EP1**（"大声失败，别静默错位"） | **改成丢弃这一笔并重新武装**（`oversize++` / `bad++`），不再 stall，见下 |

**为什么不再 stall（2026-09 改）**：stall 会让主机的写失败，而主机接下来的动作是
`clear_halt()` 重试 —— 实测这条路会把**宿主控制器**卡死：`usb_sg_wait()`/`usb_sg_cancel()`
不返回、DRM modeset 锁被占住，板子连重启都不干净。驱动靠分带保证正常主机永远不会送出
超限的 header，所以这种 header 只可能是流丢了帧同步，丢掉它比 stall 更安全；设备下一笔
照常武装，主机侧看不到错误。
（注意与 `REQ_EP1_OUT` 那条**控制请求**区分：那条仍然 stall，因为它是给老主机的明确
失败信号，不涉及 EP1 数据端点。）

`poll()` 只在解码任务空闲等待时运行（`EP1_POLL_PERIOD_MS` = 200 ms），所以它同时也是
"整笔传输超时"的唯一执行点。验收：主机侧 `S/C Bo:6 … 0 <len>`（完成状态 0，而不是 3 s 后
被驱动自己的看门狗取消成 `-104`），设备侧 `submitted/drawn` 增长且 `bad/oversize/stale`
保持 0。

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
| `0x02` | `PUD_CMD_GET_CAPS` | 填 `struct pud_caps`（**32 B**：16 字节头 + 面板参数），返回 `min(len, sizeof(caps))` |
| `0x04` | `PUD_CMD_GET_PARAM` | 填 `struct pud_param_state`（**12 B**：`settable`/`rejected` + 四个当前值），返回 `min(len, sizeof(st))` —— 见"运行期参数" |
| 其他 | — | 返回 **0**（发零长度包），主机侧读回短包 → 判定"不支持" |

### `PUD_CMD_GET_CAPS`（设备能力 + 面板参数）

主机靠这个命令知道自己一次最多能发多少字节——同一个数字决定了固件侧的
`EP1_RD_BUF_SIZE` 和帧槽大小（`PUD_MAX_TRANSFER`），而它是**按板子**定的
（RP2040 只有 256 KB SRAM，见 [pitfalls.md](pitfalls.md) 的 3.4）。这样一份主机
代码就能同时服务两种板子，不必按板重编驱动；面板参数（分辨率、旋转、bpp、总线时钟、
触摸轮询周期）也走这里，主机**不再写死 480×320**。

```c
struct pud_caps {
    u32 magic;          /* PUD_CAPS_MAGIC = 0x43445550 ("PUDC") */
    u32 proto_ver;      /* PUD_PROTO_VER = 2；不匹配主机要大声报错 */
    u32 frame_max;      /* 单次 EP1 传输上限（**含 12 字节 header**）：RP2350 65536 / RP2040 32768 */
    u32 decoder_type;   /* 0 tjpgd, 1 JPEGDEC, 2 LZ4, 3 QOI, 4 RLE */

    /* 下面这些是后来追加的：前 16 字节的布局没动，所以只读 16 字节的老主机
     * 仍然拿得到合法答复（设备按主机请求的长度截断应答）。 */
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

/* 能力位。**触摸是可选的**：pico-display-lib 里多数板级配置是
 * INDEV_DRV_NOT_USED=1（玻璃上没有控制器），主机不能因为 EP4 存在就假定有触摸。 */
#define PUD_CAPS_TOUCH 0x0001
```

**必须校验 `magic`**：没有这个命令的固件会用 EP2 缓冲里的残留内容应答
（实测：`cmd=0x7f` 曾返回上一次的 caps），所以"发回来 16 字节"不等于"支持该命令"。
驱动与 `tools/pud_usb.py` 都在 magic 不匹配时退回本机默认值（65535 B / 21835 px，
面板 480×320/旋转 0），并打印一条告警；只回了 16 字节的老固件同样按"没有面板参数"
处理（驱动日志里会写 `old firmware: no panel parameters`）。
RP2350 上 `frame_max=65536` 经 `min(65535, …)` 后与改前的编译期常量**完全一致**，
因此分带行为零变化。

## 运行期参数（`PUD_CMD_SET_PARAM` / `PUD_CMD_GET_PARAM`）

主机**向设备**写参数走的是控制端点，不新增端点：参数只有几个字节、改动很少见，而 EP0
是唯一一条"不用改配置描述符、也不跟 EP1 图像流抢带宽"的通道。EP3 继续留着做将来可能
需要的 bulk 参数/配置通道（它仍然**没有**进描述符，见上文）。

### 写入：`REQ_SET_PARAM`（0x06）+ `PUD_CMD_SET_PARAM`（0x03）

控制 OUT，数据阶段 = 查询通道那个 4 字节头 + 参数本体（共 12 字节）：

```c
struct req_set_param {
	u16 cmd; /* PUD_CMD_SET_PARAM */
	u16 size; /* sizeof(struct pud_params) */
	struct pud_params params;
};

#define PUD_PARAM_BRIGHTNESS 0x00000001 /* u8, 0..100 百分比 */
#define PUD_PARAM_ROTATION 0x00000002 /* 0..3，TFT_ROTATION 编号 */
/* 0x00000004 已退休：它原来是 fps，而设备侧根本不控节奏（EP1 流控让主机等），
 * 位号**不复用**，理由与请求号一样：已经知道这个号的实现不能被静默改语义。 */
#define PUD_PARAM_DECODER 0x00000008 /* 0..4，DECODER_TYPE 编号 */

struct pud_params {
	u32 mask; /* 本次要设置哪几个字段 */
	u8 brightness;
	u8 rotation;
	u8 reserved; /* 退休的 fps：保持结构体 8 字节、不引入隐式 padding */
	u8 decoder;
};
```

- 只有 `mask` 里点到的字段会被采纳；写本身**不返回数据**（不像查询那样回 EP2）。
- 载荷太短、或 `cmd`/`size` 不对 → 固件**stall** 这个请求（pyusb 侧会抛 `USBError`），
  而不是应用一半参数。
- 应用发生在 USB 中断里（厂商请求回调），所以这里只允许寄存器级的操作：目前唯一的
  `brightness` 就是一次 `pwm_set_gpio_level()`。将来要是有需要重初始化面板的参数，
  必须交给任务做，不能在这个上下文里做。

### 读回：`PUD_CMD_GET_PARAM`（0x04）

走现有查询通道（`REQ_EP2_IN`），回复 12 字节：

```c
struct pud_param_state {
	u32 settable; /* 这个固件能在运行期改的字段 */
	u32 rejected; /* 上一次 SET_PARAM 里没能应用的字段 */
	u8 brightness; /* 当前生效值，读取时从各自的所有者那里现取 */
	u8 rotation;
	u8 reserved;
	u8 decoder;
};
```

值都是**现读**的（`backlight_get_level()`、`g_pud_data.disp.rotation`、`DECODER_TYPE`），
不在这里缓存一份，所以查询不会报出与硬件不符的旧值。

### 现在能设什么，为什么

| 字段 | 状态 | 原因 |
| --- | --- | --- |
| `brightness` | ✅ 已实现 | `backlight_set_level()` 是一次 PWM 电平写，百分比；**读回的是主机设的值**，不是 PWM 生效值（见下） |
| `rotation` | ✅ 已实现 | 运行期改朝向 0..3，两段式落地（见下） |
| ~~`fps`~~ | **已删除** | 曾经有过一个帧率字段，后来去掉：设备侧**不控节奏**，EP1 流控让主机的 bulk 写直接等待（AGENTS.md 不变量 2），比丢帧更严格，再叠一个帧率上限只会更糟。位 `0x04` 与结构体里那个字节**保留不复用**（`reserved`），避免"已经知道这个号"的实现被静默改语义 |
| `decoder` | ❌ 上报为 rejected | `DECODER_TYPE` 决定哪些解码器被编进来（`CMakeLists.txt` + `#if`），运行期切不了 |

**`brightness` 的读回语义（实机踩过）**：pico-display-lib 的背光会在主机给的百分数上再加面板 profile 的 offset（`drivers/backlight/pwm_backlight.c` 的 `bl_lvl_offs`，本构型 **5**；
它的用意是"设 0% 也不至于完全看不见"），而 `backlight_get_level()` 返回的是**这个和**
（生效值）。第一版直接把生效值读回，实机上就是"设 10 读回 15"。现在固件缓存主机设的值并
读回它：主机拿到的是自己设的数，**面板实际会比这个数字亮 offset**；主机从没设过之前，
读回的是启动时的生效值。

**`rotation` 怎么落地的（两段式）**：`tft_set_rotation()`（pico-display-lib）就是**一次
MADCTL 写**加几何记账 —— `tft_set_addr_win()` 把逻辑窗口原样交给面板，物理映射全由 MADCTL
负责，所以不需要任何坐标变换代码，缓冲区也不必重算（一次 90° 只交换宽高，**像素总数不变**，
这也是它比 decoder 便宜的原因）。固件把它拆成两半：

本构型的具体数字（面板 ILI9488，**原生 320×480**；板级 config `pico_dm_qd3503728.cmake` 里
`TFT_HOR_RES 320 / TFT_VER_RES 480 / TFT_ROTATION 1`，显示驱动 CMakeLists 再按 1/3 交换成
逻辑 480×320）：

| 主机设 rotation | 角度 | 逻辑几何（caps 上报） |
| --- | --- | --- |
| 0 | 0°（原生） | 320×480 |
| 1 | 90°（编译期默认） | 480×320 |
| 2 | 180° | 320×480 |
| 3 | 270° | 480×320 |

规则就是"奇偶与编译期朝向不同就交换宽高"（`tft_set_rotation()` 里那一行判断），方向沿用 lib
里 `set_dir()` 既有的 MADCTL 表，所以主机设 1 得到的就是 config 里那个朝向本身。

1. **USB 中断里只做记账**：更新 `g_pud_data.disp.rotation` 与 `xres/yres`（**caps 立刻按新
   几何上报** —— 主机正是靠回读 caps 来定 DRM mode，慢一拍就会建错 mode），并调
   `indev_set_dir(indev_dir_for_rotation())` 让触摸跟着走（纯记账，中断里安全）；
2. **MADCTL 写留给 `decoder_task`**（`pud_params_flush_display()`）：面板寄存器写要走总线，
   不能在 USB 中断里做。任务在**画每一帧之前**、以及空闲循环里都会调用它，所以"下一帧"
   必然落在新朝向下；面板只有一个写者（这个任务），两者不会在面板面前打架。

**bootlogo 在"运行期朝向 ≠ 编译期朝向"时直接跳过**（`decoder_draw_bootlogo()` 开头
return）：logo 是为编译朝向烘焙的一整张图，没有第二份，不画比画歪强 —— 主机随后发的第一帧
就是它要的画面。

**"不支持"要如实上报、不能装作成功**：主机的写不会失败，`rejected` 位会告诉它哪几个
字段没生效。这也让协议可以慢慢长——加一个字段不需要改已有字段的语义。

**老固件怎么表现**：没有 `PUD_CMD_GET_PARAM` 的固件对未知 cmd 返回 0 长度，主机读回短包
就该判"设备不支持运行期参数"（和查询通道既有的规则一致）；`REQ_SET_PARAM` 则会被
`default:` 分支 stall。两边都不会静默错解。

### 用户态验证（不需要内核驱动）

```bash
python3 tests/param_test.py              # 自检：调暗/调亮并读回，再验不支持字段被上报
python3 tests/param_test.py --brightness 30   # 只设一个值并读回
```

`pud_usb.py` 里的 `Display.set_params()` / `Display.get_params()` 是这两个命令的实现，
`PARAMS_STRUCT` / `PARAM_STATE_STRUCT` 的布局与固件的 `_Static_assert` 是**同一份约定**
（8 / 12 字节，无 padding）。

> 驱动侧（`PUD-kernel-drivers`）**已实现这两个命令**：`pud_set_params()` /
> `pud_push_params()` 在 probe 时下发模块参数 `brightness=`/`rotation=`/`decoder=`
> （`-1` = 不动），并把 `rejected` 打出来；驱动仓的
> `notes/usb-protocol.md` 有同一份契约的镜像段落。

## EP4 触摸的处理

**设备主动推送**，主机常挂一个 interrupt IN 的 URB（输入端点的正常形态）：

```c
/* 触摸任务每 tp.polling_period(10) ms 轮询一次控制器 */
pressed = indev_is_pressed();
if (pressed || 刚刚松开) {
    report = ...;                      /* 见下 */
    usbd_vendor_ep4_submit(&report);   /* 空闲则武装 EP4，在途则覆盖 */
}
```

**采样率由两个旋钮共同决定**，只调一个没用：

| 旋钮 | 值 | 位置 |
| --- | --- | --- |
| 触摸轮询周期 | `INDEV_POLLING_PERIOD_MS = 10`（100 Hz） | 固件 `CMakeLists.txt`（覆盖库板级配置里的 33） |
| EP4 `bInterval` | `EP4_POLL_INTERVAL_MS = 8`（主机最多每 8 ms 来取一次） | `src/cherryusb/usbd_vendor.h` |

实测（2026-09，真机拖动）：33 ms 轮询 + 33 ms `bInterval` 那一版，相邻坐标点间隔 **32 ms**
（正好卡在 `bInterval` 上——主机不来取，固件推得再快也只会覆盖合并）；改成 10 ms + 8 ms 后
同一条拖动路径量到中位 **8.0 ms（≈125 Hz）**，一次 7.5 s 拖动 563 个坐标点、松手收尾正常。
FT6236 自己的扫描周期默认约 12 ms（`PERIODACTIVE`），所以 10 ms 轮询不会再被控制器拖慢。

**代价实测为零**：`fps_bench.py --full --frames 30` 同一主机同一脚本，改前 / 改后
（MB/s）—— gradient 分带 1.096 / **1.096**、gradient 单次传输 1.129 / **1.129**、
photo 1.104 / **1.105**、noise 1.133 / **1.133**。8 ms 的周期端点在总线上占的带宽对
EP1 没有可测影响（理论上也只有 8 B × 125 Hz 的量级）。

`REQ_EP4_IN`（0x05）**保留**：它把**当前**这一帧发给主机，给"轮询式"主机用
（旧驱动就是这么写的）。两种模型共存，互不干扰。**松手兜底仍未实现**：设备只在按下/移动/
松手时发帧，空闲不发，所以主机如果想"多久没收到按下帧就认为松开"，得自己加超时。

**没有触摸驱动的固件**（`INDEV_DRV_NOT_USED=1`，configs 里多数如此）：触摸任务
（`main.c` 里 `#if !INDEV_DRV_NOT_USED`）根本不创建，EP4 永远不发帧，
`tp.polling_period` 报 0、capability 里 `PUD_CAPS_TOUCH` 不置位，而且
`usbd_vendor_ep4_reset()` 把报告的 `version` 留成 **0**（老主机在 poll 模式下据此判定
"这版固件没实现触摸"）。主机侧据此**不注册输入设备** —— 两种情况都已上机验证：
有触摸时 `touch yes` + 注册 input，无触摸时 `touch no` + 日志
`device reports no touch controller, not registering input`、`/proc/bus/input/devices`
与 libinput 里都没有该设备。

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

两者正好相差 180°，说明变换确实由旋转推导。顺带验证：按下期间每 33 ms 一帧（当时是
33 ms 轮询 + 33 ms `bInterval` 那一版配置）、`version=1`、空闲时主机读会超时（设备不发声）。

**真实手指（2026-09，`touch_draw.py --mode grid --rate 20`）**：29 次触摸、193 个报告、
29 个 release（每次触摸都有收尾，没有卡在按下）：

- 四角落点 (0,0)、(475,0)、(475,319)、(0,314) —— 象限正确，**无轴交换/反向**（电容屏+
  贴合偏移，摸不到 479/319 是正常的，固件端已钳位，实测无越界值）；
- 横拖只变 x（`y=195` 恒定，x 48..371）、竖拖只变 y（`x=234` 恒定，y 42..298）；
- 全程 sequence 只跳了 **2 次**（即 2 个样本被合并：设备在上一帧还在途时用最新样本覆盖，
  这是设计行为）；面板更新限速 20 Hz 时，**板子 10 分钟没有挂**。

> 空闲时**不发**任何帧，所以主机不要指望"心跳"。驱动侧如果担心丢失"松手"那一帧，
> 自己加超时判定（**至今未实现**，见本文件 EP4 一节末尾）。

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
2. **`size` 可能比真实压缩长度大 1**（主机可以选择向上取偶；设备奇偶都接受）。
   QOI 解码在结束标记处停止，不会消费多余字节。

详见 `PUD-kernel-drivers/notes/usb-protocol.md` 的两节"已知不一致"。

## 改协议时的检查清单

1. `src/cherryusb/usbd_vendor.c` 的结构体与 `vendor_request_handler()`
2. `src/cherryusb/usbd_vendor.h` 的 `REQ_*` / 端点地址 / 缓冲尺寸
3. 驱动 `usb.c` 的对应结构体与 `pud_feed_ctrl_buf()`
4. 两侧 notes 里的协议文档
