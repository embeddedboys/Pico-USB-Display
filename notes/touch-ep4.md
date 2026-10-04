# EP4 触摸通道（设备侧协议）

> 触摸是**设备主动推送**：主机常挂一个 interrupt IN 的 URB；采样率由"轮询周期 10 ms"与
> "EP4 `bInterval` 8 ms"两个旋钮共同决定，只调一个没用。

## TL;DR

- 每份报告 **8 字节**、逐字节定义（flags / 大端 x / 大端 y / sequence / version / reserved）；前 5 字节与旧驱动解析的布局完全一致。
- 设备只在按下/移动/松手时发帧，**空闲不发**；同一时刻只允许一帧在途，新样本**覆盖**旧样本。
- `REQ_EP4_IN`（0x05）保留给轮询式主机；推送与轮询两种模型共存。
- 触摸控制器本身的轴序/反向/偏移**不在驱动里做**，全在 `pico-display-lib` 的 `indev.c` 按 `TFT_ROTATION` 变换（见 [../AGENTS.md](../AGENTS.md) 不变量 10）。

## 处理流程

```c
/* 触摸任务每 tp.polling_period(10) ms 轮询一次控制器 */
pressed = indev_is_pressed();
if (pressed || 刚刚松开) {
    report = ...;                      /* 见下 */
    usbd_vendor_ep4_submit(&report);   /* 空闲则武装 EP4，在途则覆盖 */
}
```

**采样率由两个旋钮共同决定**：

| 旋钮 | 值 | 位置 |
| --- | --- | --- |
| 触摸轮询周期 | `INDEV_POLLING_PERIOD_MS = 10`（100 Hz） | 固件 `CMakeLists.txt`（覆盖库板级配置里的 33） |
| EP4 `bInterval` | `EP4_POLL_INTERVAL_MS = 8`（主机最多每 8 ms 来取一次） | `src/cherryusb/usbd_vendor.h` |

实测（2026-09，真机拖动）：33 ms 轮询 + 33 ms `bInterval` 那一版，相邻坐标点间隔 **32 ms**
（正好卡在 `bInterval` 上——主机不来取，固件推得再快也只会覆盖合并）；改成 10 ms + 8 ms 后
同一条拖动路径量到中位 **8.0 ms（≈125 Hz）**，一次 7.5 s 拖动 563 个坐标点、松手收尾正常。
FT6236 自己的扫描周期默认约 12 ms（`PERIODACTIVE`），所以 10 ms 轮询不会再被控制器拖慢。

**代价实测为零**：`fps_bench.py --full --frames 30` 同一主机同一脚本，改前 / 改后（MB/s）——
gradient 分带 1.096 / **1.096**、gradient 单次传输 1.129 / **1.129**、photo 1.104 / **1.105**、
noise 1.133 / **1.133**。8 ms 的周期端点在总线上占的带宽对 EP1 没有可测影响。

**松手兜底仍未实现**：设备只在按下/移动/松手时发帧，空闲不发，所以主机如果想"多久没收到按下帧
就认为松开"，得自己加超时。

## 上报格式（8 字节，逐字节定义）

| 字节 | 含义 |
| --- | --- |
| 0 | flags，bit0 = 按下 |
| 1–2 | x >> 8、x & 0xff（大端） |
| 3–4 | y >> 8、y & 0xff |
| 5 | sequence（回绕；跳变说明主机漏采/被合并） |
| 6 | version = `PUD_TOUCH_VERSION`(1)，主机据此判断这版固件真的支持触摸 |
| 7 | 保留，0 |

前 5 个字节与"旧驱动已经在解析"的布局**完全一致**（`buf[0]` 按下、`buf[1..2]` x、`buf[3..4]` y），
所以驱动的字段偏移不用改；多出来的是 sequence/version。**同一时刻只允许一帧在途**，期间到来的
新样本**覆盖**旧的（只保留最新状态，排队只会加延迟）；传输完成回调里若发现有更新的样本会立即补发。

**坐标是显示坐标系下的面板坐标**（0..479 / 0..319），由库按 `TFT_ROTATION` 变换，与面板驱动
用的是同一个数字；固件侧再钳一次，保证上线值不越界。

`REQ_EP4_IN`（0x05）**保留**：它把**当前**这一帧发给主机，给"轮询式"主机用（旧驱动就是这么写的）。
两种模型共存，互不干扰。

## 没有触摸驱动的固件

`INDEV_DRV_NOT_USED=1`（configs 里多数如此）：触摸任务根本不创建，EP4 永远不发帧，
`tp.polling_period` 报 0、capability 里 `PUD_CAPS_TOUCH` 不置位，而且 `usbd_vendor_ep4_reset()`
把报告的 `version` 留成 **0**（老主机在 poll 模式下据此判定"这版固件没实现触摸"）。主机侧据此
**不注册输入设备** —— 两种情况都已上机验证：有触摸时 `touch yes` + 注册 input，无触摸时
`touch no` + 日志 `device reports no touch controller, not registering input`、
`/proc/bus/input/devices` 与 libinput 里都没有该设备。

## 已验证（2026-09）

**SWD 强制控制器原始值**：用 gdb 把 `ft6236_read_x/read_y` 的返回值钉成 `raw_x=100, raw_y=200`
（`is_pressed` 强制为 1），从主机读 EP4：

| `TFT_ROTATION` | 上报 | 对应关系 |
| --- | --- | --- |
| 1 | (200, 219) | x = raw_y，y = 319 - raw_x |
| 3 | (279, 100) | x = 479 - raw_y，y = raw_x |

两者正好相差 180°，说明变换确实由旋转推导。顺带验证：按下期间每 33 ms 一帧（当时是 33 ms 轮询
+ 33 ms `bInterval` 那一版配置）、`version=1`、空闲时主机读会超时（设备不发声）。

**真实手指（`touch_draw.py --mode grid --rate 20`）**：29 次触摸、193 个报告、29 个 release
（每次触摸都有收尾，没有卡在按下）：

- 四角落点 (0,0)、(475,0)、(475,319)、(0,314) —— 象限正确，**无轴交换/反向**（电容屏 + 贴合
  偏移，摸不到 479/319 是正常的，固件端已钳位，实测无越界值）；
- 横拖只变 x（`y=195` 恒定，x 48..371）、竖拖只变 y（`x=234` 恒定，y 42..298）；
- 全程 sequence 只跳了 **2 次**（即 2 个样本被合并：设备在上一帧还在途时用最新样本覆盖，
  这是设计行为）；面板更新限速 20 Hz 时，**板子 10 分钟没有挂**。

## 相关

- 主机侧协议（EP1/EP2/控制请求）：[usb-protocol.md](usb-protocol.md)
- 触摸测试脚本：[scripts.md](scripts.md)
