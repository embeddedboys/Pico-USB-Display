# picoboot 复位接口（第二个 USB 接口）

> 配置描述符里的第 2 个接口（`0xFF/0x00/0x01`，无端点）是 Raspberry Pi 约定的 reset 接口，
> 让主机请设备重启进 BOOTSEL 或重启回应用 —— 应用态就能烧写，不用按按钮。

## TL;DR

- 接口 1 = `RESET_INTERFACE_SUBCLASS 0x00` / `RESET_INTERFACE_PROTOCOL 0x01`；接口 0 才是 PUD 图像协议。
- `bRequest=0x01`（BOOTSEL）→ `reset_usb_boot(0, wValue & 0x7f)`，**不返回**；`bRequest=0x02`（FLASH）→ `watchdog_reboot()`。
- picotool 必须显式给 `--vid 0x2e8a --pid 0x0001`，且设备选择选项写在命令选项前。
- 它不是 PUD 协议的一部分，但**不是透明的**：内核驱动必须只匹配图像接口，否则会为每个接口各 probe 一次。

## 接口

配置描述符有**两个接口**（`bNumInterfaces = 2`）：

| 接口 | 类/子类/协议 | 端点 | 作用 |
| --- | --- | --- | --- |
| 0 | `0xFF / 0x00 / 0x00` | EP1 OUT、EP2 IN、EP4 IN | 图像 / 查询 / 触摸，见 [usb-protocol.md](usb-protocol.md) |
| 1 | `0xFF / 0x00 / 0x01` | 无 | **picoboot 的 reset 接口**：主机请设备重启 |

接口 1 的类码是 Raspberry Pi 的约定，不是我们定的 —— SDK 的 `pico_stdio_usb` 用的就是同一个
（`src/common/pico_usb_reset_interface_headers/include/pico/usb_reset_interface.h`）：

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
入口再挡一道。

> 实测面：上面两条请求都在 **RP2040** 板上跑过（第二个接口与 `reset_usb_boot` 走的就是 SDK 的
> 实现）。**RP2350 只编过、没在真机验证**（走的是 SDK 里 `reset_usb_boot()` 的另一条实现
> `rom_reboot`，两者接口一致，但没实测）。

## 相关

- 烧录方式与 picotool 的 PID 过滤细节：[build-and-flash.md](build-and-flash.md)
- 主机侧协议（接口 0）：[usb-protocol.md](usb-protocol.md)
