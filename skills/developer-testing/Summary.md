# Summary

测试与工具分三层：**`tests/` 解释观察**（显式 oracle → PASS/FAIL/INCONCLUSIVE），
**`tools/` 只产出事实**（确定性、可脚本化、机器可读、不内嵌项目结论），最下面是设备/系统。
`tests/` 调用 `tools/`，不反过来。

**复用先于创建**：新建脚本前先搜 `tests/`、`tools/`、`scripts/`；能扩展既有工具就不新建。
**禁止 measurement-as-specification**：某次观测到的值不能自动变成期望值或 golden 文件；
期望值必须有**可指认的来源**，并在测试里显式声明。

**Oracle 声明**：每个产生 PASS/FAIL 的测试都要标注类型与来源，形如
`# ORACLE: SPEC / # SOURCE: notes/xxx.md / # EXPECTED: 95..105 Hz`。
类型有 `SPEC` / `REQUIREMENT` / `INVARIANT` / `RELATIONSHIP` / `GOLDEN` / `BASELINE` /
`USER_DEFINED` / `NONE`。**没有可靠 oracle 时用 `NONE` 并报 `INCONCLUSIVE`**，
**不要编造阈值或容差**。

**CLI 与退出码**：统一 `工具名 <子命令> [选项]`，支持
`--help/--version/--json/--timeout/--quiet/--verbose`。
退出码：`0` PASS/成功、`1` FAIL、`2` INVALID_USAGE、`3` ENVIRONMENT_ERROR、
`4` TIMEOUT、`5` INCONCLUSIVE。**硬件缺失或无权限是 ENVIRONMENT_ERROR，不是 FAIL**。

**噪声测量要有显式方法学**（N 次、median、离散度），
**不要用单次采样支撑统计结论**；也不要把"跨条件差异"当成"重复性噪声"来用。
测试自己创建的资源要清理；硬件测试要显式定义**复位与就绪检测**，
不要依赖"设备恰好已经跑着"。

**命名**：工具用稳定功能名 `<domain>ctl`（如 `pudctl`）；测试用 `test_<被测行为>`。
禁止 `test_new`、`test_final2`、`tmp` 之类名字。

**离线优先**：验证顺序是**宿主机/离线 → 真实硬件**。真机测试遵守各仓库的硬件纪律
（回读比对、读日志先于烧写、调试会话以 `reset run` 收尾等）。

**Oracle 的边界**：oracle 必须来自**可指认的外部依据**（规范、需求、不变量、关系、
已记录的基线），不能来自"上次测出来是多少"。平台换了（速率、位宽、flash 类型不同），
**旧 oracle 就不再适用**——此时要么给出新平台的规范依据，要么用 `NONE` 报 `INCONCLUSIVE`，
**不要为了让测试变绿而调阈值**。

**测试要能自证跑对了**：被测行为要有明确的**就绪检测**与**复位路径**，
失败时要能区分"设备没准备好"（`ENVIRONMENT_ERROR`）、"超时"（`TIMEOUT`）、
"没结论"（`INCONCLUSIVE`）与真正的 `FAIL`。
测试自身创建的资源（进程、文件、设备状态、临时目录）**必须清理**，
不留副作用给下一次运行。

**真机纪律**：回读比对而不是只信写入成功；**读日志先于烧写**；
调试会话以复位收尾；不要在设备恰好处于某状态时开始测试。
一次测量只改变一个变量，**先排除干扰项**（例如跑着的高负载任务），
而不是事后用噪声去论证"可以忽略"。
