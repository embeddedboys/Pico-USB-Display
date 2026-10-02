# tests/ —— 验证脚本

每个测试做一件事：**收集观察 → 对照显式 oracle → 给出 PASS / FAIL / INCONCLUSIVE**。
共用基础设施在 [`common/harness.py`](common/harness.py)（CLI、退出码、oracle 声明、设备打开），
测量原语在 [`../tools/measure.py`](../tools/measure.py)，协议/设备访问在
[`../tools/pud_usb.py`](../tools/pud_usb.py)。**不要**在这里再实现一遍设备通信或编码器。

## 退出码（全工作区统一）

```text
0 PASS   1 FAIL   2 INVALID_USAGE   3 ENVIRONMENT_ERROR   4 TIMEOUT   5 INCONCLUSIVE
```

设备缺失/无权限/依赖缺失 = **3**，不是 FAIL。没有可靠 oracle 的用例报 **5**。
所有测试都支持 `--help / --version / --json / --timeout / --quiet / --verbose`。

## 离线（不需要设备，也不需要 numpy / Pillow，除注明外）

```bash
python3 tests/test_encoder_reference.py     # Python QOI/RLE 与 C 库参考向量
python3 tests/test_protocol_constants.py    # 协议常量与构建开关：文档/镜像 vs 源码
python3 tests/test_runner_contract.py       # 测试基础设施自身的契约 + 各真机用例的判定逻辑（桩设备）

# 需要 numpy + Pillow + numpy 的 lz4（.venv 里齐了），以及先构建一次 C 工具：
cmake -S tools -B tools/build && cmake --build tools/build
.venv/bin/python tests/test_codec_crosscheck.py   # pudcodec vs Python 编码器 vs bootlogo.h
```

## 真机（可选；检测不到设备报 3）

设备不能被 `pud` 内核驱动占用，装 `60-pico-usb-display.rules` 可免 root。

```bash
python3 tests/test_ep2_query.py            # EP2 查询通道 + caps 自洽性
python3 tests/test_ep1_throughput.py       # EP1 吞吐扫描（真 QOI 载荷，编码在计时外）
python3 tests/test_param_channel.py        # 运行期参数：写回读、rejected 掩码
python3 tests/test_rotation_geometry.py    # 旋转 → caps 几何 → 整帧送达
python3 tests/test_touch_ep4.py            # EP4 触摸上报契约（需要手指；空闲=5）
```

## Oracle 来源

| 测试 | oracle | 来源 |
| --- | --- | --- |
| `test_encoder_reference` | GOLDEN | C 库参考向量（`rgb565-qoi` / `rgb565-rle`） |
| `test_protocol_constants` | SPEC | `include/pud.h`、`usbd_vendor.h`、`CMakeLists.txt` |
| `test_codec_crosscheck` | RELATIONSHIP | 两条主机路径是同一个编解码器（`notes/scripts.md`） |
| `test_runner_contract` | REQUIREMENT | 工作区 `AGENTS.md` 的退出码与 oracle 规则 |
| `test_ep2_query` | SPEC | `notes/usb-protocol.md`（查询表 / caps / 旋转几何表） |
| `test_ep1_throughput` | SPEC | USB 2.0 全速 bulk 上限 1.216 MB/s（无下限断言） |
| `test_param_channel` | SPEC | `notes/usb-protocol.md` 运行期参数一节 |
| `test_rotation_geometry` | SPEC | `notes/usb-protocol.md` 旋转→几何表 |
| `test_touch_ep4` | SPEC | `notes/usb-protocol.md` EP4 上报布局 |

真机用例的判定逻辑由 `test_runner_contract.py` 用桩设备在离线下覆盖，
所以"只能在板上跑"的代码也有回归保护。
