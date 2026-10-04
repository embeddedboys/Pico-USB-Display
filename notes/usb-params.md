# 运行期参数通道（SET_PARAM / GET_PARAM）

> 主机**向设备**写运行期参数走控制端点（EP0），不新增端点；参数只有几个字节、改动很少见，
> 而 EP0 是唯一一条"不用改配置描述符、也不跟 EP1 图像流抢带宽"的通道。

## TL;DR

- 写：`REQ_SET_PARAM`（0x06）+ `PUD_CMD_SET_PARAM`（0x03），数据阶段 = 4 字节头 + `struct pud_params`（共 12 字节）。
- 读：`PUD_CMD_GET_PARAM`（0x04）走 EP2 查询通道，回复 `struct pud_param_state`（12 字节）。
- 目前只有 `brightness` 与 `rotation` 可设；`decoder` 上报为 rejected；`fps`（位 `0x04`）已退休且**不复用**。
- 应用发生在 USB 中断里，只允许寄存器级操作；面板寄存器写由 `decoder_task` 完成（两段式）。

## 写入：`REQ_SET_PARAM`（0x06）+ `PUD_CMD_SET_PARAM`（0x03）

控制 OUT，数据阶段 = 查询通道那个 4 字节头 + 参数本体（共 12 字节）：

```c
struct req_set_param {
	u16 cmd; /* PUD_CMD_SET_PARAM */
	u16 size; /* sizeof(struct pud_params) */
	struct pud_params params;
};

#define PUD_PARAM_BRIGHTNESS 0x00000001 /* u8, 0..100 百分比 */
#define PUD_PARAM_ROTATION   0x00000002 /* 0..3，TFT_ROTATION 编号 */
/* 0x00000004 已退休（原 fps）；设备侧根本不控节奏，位号**不复用** */
#define PUD_PARAM_DECODER    0x00000008 /* 0..5，DECODER_TYPE 编号 */

struct pud_params {
	u32 mask; /* 本次要设置哪几个字段 */
	u8 brightness;
	u8 rotation;
	u8 reserved; /* 退休的 fps：保持结构体 8 字节、不引入隐式 padding */
	u8 decoder;
};
```

- 只有 `mask` 里点到的字段会被采纳；写本身**不返回数据**。
- 载荷太短、或 `cmd`/`size` 不对 → 固件**stall** 这个请求（pyusb 侧会抛 `USBError`），而不是
  应用一半参数。
- 应用发生在 USB 中断里，所以这里只允许寄存器级的操作：目前唯一的 `brightness` 就是一次
  `pwm_set_gpio_level()`。将来要是有需要重初始化面板的参数，**必须交给任务做**。

## 读回：`PUD_CMD_GET_PARAM`（0x04）

走查询通道（`REQ_EP2_IN`），回复 **12 字节**：

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

## 能设什么，为什么

EP3 继续留着做将来可能需要的 bulk 参数/配置通道（它仍然**没有**进描述符）。

值都是**现读**的（`backlight_get_level()`、`g_pud_data.disp.rotation`、`DECODER_TYPE`），不缓存，
所以查询不会报出与硬件不符的旧值。

| 字段 | 状态 | 原因 |
| --- | --- | --- |
| `brightness` | ✅ 已实现 | `backlight_set_level()` 是一次 PWM 电平写；**读回的是主机设的值**，不是 PWM 生效值 |
| `rotation` | ✅ 已实现 | 运行期改朝向 0..3，两段式落地 |
| ~~`fps`~~ | **已删除** | 设备侧**不控节奏**，EP1 流控让主机的 bulk 写直接等待。位 `0x04` 与结构体里那个字节**保留不复用** |
| `decoder` | ❌ 上报为 rejected | `DECODER_TYPE` 决定哪些解码器被编进来，运行期切不了 |

**`brightness` 的读回语义（实机踩过）**：pico-display-lib 的背光会在主机给的百分数上再加面板
profile 的 offset（`drivers/backlight/pwm_backlight.c` 的 `bl_lvl_offs`，本构型 **5**；用意是
"设 0% 也不至于完全看不见"），而 `backlight_get_level()` 返回的是**这个和**。第一版直接把生效值
读回，实机上就是"设 10 读回 15"。现在固件缓存主机设的值并读回它：**面板实际会比这个数字亮
offset**；主机从没设过之前，读回的是启动时的生效值。

**`rotation` 怎么落地的（两段式）**：`tft_set_rotation()`（pico-display-lib）就是**一次 MADCTL 写**
加几何记账 —— `tft_set_addr_win()` 把逻辑窗口原样交给面板，物理映射全由 MADCTL 负责，所以不需要
任何坐标变换代码，缓冲区也不必重算（一次 90° 只交换宽高，**像素总数不变**）。
本构型（ILI9488，**原生 320×480**，`TFT_ROTATION 1` → 逻辑 480×320）的对应关系：

| 主机设 rotation | 角度 | 逻辑几何（caps 上报） |
| --- | --- | --- |
| 0 | 0°（原生） | 320×480 |
| 1 | 90°（编译期默认） | 480×320 |
| 2 | 180° | 320×480 |
| 3 | 270° | 480×320 |

规则就是"奇偶与编译期朝向不同就交换宽高"，方向沿用 lib 里 `set_dir()` 既有的 MADCTL 表。
固件把它拆成两半：

1. **USB 中断里只做记账**：更新 `g_pud_data.disp.rotation` 与 `xres/yres`（**caps 立刻按新几何
   上报** —— 主机正是靠回读 caps 来定 DRM mode，慢一拍就会建错 mode），并调
   `indev_set_dir(indev_dir_for_rotation())` 让触摸跟着走（纯记账，中断里安全）；
2. **MADCTL 写留给 `decoder_task`**（`pud_params_flush_display()`）：面板寄存器写要走总线，不能
   在 USB 中断里做。任务在**画每一帧之前**、以及空闲循环里都会调用它，所以"下一帧"必然落在新
   朝向下；面板只有一个写者（这个任务）。

**bootlogo 在"运行期朝向 ≠ 编译期朝向"时直接跳过**（`decoder_draw_bootlogo()` 开头 return）：
logo 是为编译朝向烘焙的一整张图，没有第二份，不画比画歪强。

**"不支持"要如实上报、不能装作成功**：主机的写不会失败，`rejected` 位会告诉它哪几个字段没生效。
**老固件**对未知 cmd 返回 0 长度，主机读回短包就该判"设备不支持运行期参数"；`REQ_SET_PARAM`
则会被 `default:` 分支 stall。两边都不会静默错解。

用户态验证：`python3 tests/test_param_channel.py`（自检 + `--brightness 30`）；
`pud_usb.py` 的 `Display.set_params()` / `get_params()` 与固件的 `_Static_assert` 是**同一份约定**
（8 / 12 字节，无 padding）。驱动仓**已实现这两个命令**（probe 时下发模块参数
`brightness=`/`rotation=`/`decoder=`，`-1` = 不动，并把 `rejected` 打出来）。

## 相关

- 设备侧协议总览（EP1/EP2/控制请求）：[usb-protocol.md](usb-protocol.md)
- 触摸通道：[touch-ep4.md](touch-ep4.md)
- 参数测试脚本：[scripts.md](scripts.md)
