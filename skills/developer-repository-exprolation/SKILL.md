# Developer Repository Exploration Skill

## Purpose

在进入一个陌生工程、代码仓库或已有项目时，先建立足够可靠的工程模型，再开始修改代码。

本 Skill 解决以下常见问题：

* 只阅读当前文件或当前函数，就开始修改代码
* 根据函数名、变量名或经验猜测系统行为
* 不搜索 caller / callee / callback 就修改接口
* 不理解生命周期就修改初始化、退出或资源释放逻辑
* 不理解 build system 就添加文件、依赖或配置
* 不理解现有实现模式就重新发明一套实现
* 忽略 Device Tree、Kconfig、Makefile、配置文件等外部约束
* 只看到局部代码，却把局部理解当成整个系统事实
* 遇到不确定内容时继续“合理猜测”
* 一边修改代码，一边才逐渐发现原来的理解错误
* 最终形成“能运行，但架构和维护性很差”的代码

核心原则：

> **先理解，再修改。**
>
> **Evidence before assumption.**
>
> **Repository evidence before personal intuition.**

---

# 1. Core Rules

## 1.1 Do not code immediately

面对一个陌生 repository 时，默认进入：

```text
RECONNAISSANCE
    ↓
MODEL
    ↓
IMPLEMENT
    ↓
VERIFY
```

而不是：

```text
READ ONE FILE
    ↓
GUESS
    ↓
EDIT
    ↓
COMPILE
    ↓
PATCH
    ↓
GUESS AGAIN
```

在完成必要的工程侦察之前，不应修改生产代码。

---

## 1.2 Do not confuse local context with system context

当前文件中的代码只能说明局部行为。

不能因为看到：

```c
foo_init();
```

就假设：

* `foo_init()` 一定只被调用一次
* 调用者拥有资源
* 调用失败可以忽略
* 函数可以阻塞
* 函数可以睡眠
* 函数执行后状态一定发生变化

必须继续寻找：

* definition
* callers
* callees
* callbacks
* related structures
* initialization path
* teardown path
* error path
* tests
* configuration

---

## 1.3 Evidence before assumption

实现行为时，优先级：

```text
Actual repository code
        >
Repository tests
        >
Build/configuration
        >
Project documentation
        >
Authoritative external documentation
        >
General engineering knowledge
        >
Personal assumption
```

不能用经验替代 repository 中已经存在的事实。

例如不要因为：

```text
Linux driver 通常这样做
```

就直接修改驱动。

应该先搜索项目中已有的类似实现。

---

## 1.4 Unknown must remain unknown

如果某个行为尚未确认，应明确视为：

```text
UNKNOWN
```

而不是：

```text
PROBABLY ...
LIKELY ...
SHOULD ...
I ASSUME ...
```

正确流程：

```text
Unknown
  ↓
Search
  ↓
Read definition / caller / documentation / test
  ↓
Evidence
  ↓
Conclusion
```

如果仍然无法确认：

```text
Unknown
  ↓
Document uncertainty
  ↓
Choose the smallest safe change
```

---

## 1.5 Bring up the official example first

碰到新平台、新外设、新协议栈、没做过的事：`RECONNAISSANCE → MODEL` 之后**不要直接开始写自己的
代码** —— 先**跑通上游官方例子**（厂商 SDK / 官方仓库里那个 demo），用**它自己的输出**证明：

```text
toolchain
flash / boot path
wiring
clock / reset
power
link (USB / WiFi / bus)
```

这些东西都是好的，才算拿到了**基线**；此后自己的代码 = 官方例子 + **最小 delta**。

- **复用官方那份构建配方**：`CMakeLists` / `Kconfig` / `sdkconfig`、官方引脚与初始化顺序照搬。
- **官方例子源码逐字不动**（放 `third_party/`，不改写）；差异用 wrapper / 独立文件 / 编译选项承载，
  每处 delta 都能指认"改了哪一行 / 哪个选项、为什么"，并且单独验证。
- **不许凭经验把官方初始化流程重写一遍** ✗ —— 那会让"我的代码写错了"和"硬件/链路本来就不对"
  混在一起，两边的结论都不可信。

官方例子跑不通时，先按**仪器问题**处理：

- **先怀疑仪器**（串口读者、量具、烧写、接线、常驻进程的生死），**不要据此宣布"硬件坏了"** ✗。
  量具要能自证：读者自报读到的字节数、**先开读者再复位**、常驻监听用受管后台作业
  （`setsid … &` 会随调用一起被杀）。
- **官方例子本身也要验**：长期没人编的分支可能根本编不过（缺宏）；看起来正常的调用可能
  **从未执行**。判据是**观察到的行为**，不是"代码长这样"。

真实事故：某个官方例子的客户端模式同时坏在两处 —— 缺一个宏导致编不过；唯一的启动调用被写在
`assert()` 里，Release（`-DNDEBUG`）下整句被删掉 ⇒ 静默一个包都没发。而上游 CMakeLists 从来只建
服务端 target ⇒ 这条分支长期无人编译。**"官方例子"不等于"能跑的代码"。**

---

# 2. Repository Exploration Workflow

## Phase 0 — Task Understanding

首先明确用户真正要求改变的是什么。

至少确认：

* What is the requested behavior?
* What is the expected result?
* Which subsystem appears relevant?
* Is this a bug fix, feature, refactor, optimization, test, or investigation?
* What constraints are explicitly stated?
* What behavior must remain unchanged?

不要在这一阶段提前设计实现方案。

---

# 3. Phase 1 — Repository Reconnaissance

## 3.1 Inspect repository structure

首先获得仓库整体结构。

检查：

```text
top-level directories
source directories
include directories
tests
examples
scripts
tools
documentation
configuration
build files
CI files
generated files
submodules
```

常见命令：

```bash
ls
find . -maxdepth 2 -type f
find . -maxdepth 2 -type d
```

根据工程大小选择合理深度。

不要机械地读取整个 repository。

目标是建立：

```text
Repository
├── source
├── headers
├── tests
├── tools
├── configuration
├── documentation
└── build system
```

---

## 3.2 Identify the build system

必须确认工程如何构建。

例如：

```text
Make
CMake
Meson
Bazel
Cargo
Kconfig
Linux kernel kbuild
Yocto
Buildroot
Gradle
npm
custom scripts
```

寻找：

```text
Makefile
CMakeLists.txt
meson.build
Cargo.toml
Kconfig
Kbuild
build.gradle
package.json
build.sh
configure
```

不要假设：

```bash
make
```

就是正确构建方式。

必须找到项目实际使用的 build path。

---

## 3.3 Identify entry points

根据项目类型寻找：

### Application

```text
main()
CLI entry
service entry
daemon entry
```

### Library

```text
public API
initialization API
registration API
factory
```

### Linux kernel

```text
module_init
module_exit
probe
remove
subsys_initcall
device registration
platform driver
USB driver
PCI driver
I2C driver
SPI driver
DRM driver
V4L2 driver
IIO driver
```

### Firmware

```text
reset handler
main()
RTOS task
interrupt vector
startup code
USB descriptors
protocol entry points
```

### Web application

```text
server entry
route registration
frontend entry
API handlers
```

目标不是立即理解所有代码，而是找到：

> **系统从哪里开始运行。**

---

# 4. Phase 2 — Build the Repository Map

在开始实现前，建立一个简洁的工程地图。

至少回答：

```text
Project purpose:
Build system:
Main entry point:
Major subsystems:
Relevant subsystem:
Relevant configuration:
Relevant tests:
Relevant external dependencies:
```

例如：

```text
Project:
USB display kernel driver

Build:
Linux kbuild

Entry:
usb_driver.probe()

Major subsystems:
USB transport
Framebuffer/DRM
Buffer management
Userspace interface

Relevant path:
USB probe
  → device initialization
  → framebuffer setup
  → framebuffer update
  → USB bulk transfer

Relevant configuration:
Kconfig
Kbuild
Device USB IDs

Tests:
userspace framebuffer test
manual USB transfer test
```

这个地图不需要成为长文档。

目标是：

> **建立方向，而不是制造新的知识库垃圾。**

---

# 5. Phase 3 — Trace the Relevant Dependency Closure

不要要求阅读整个 repository。

应针对当前任务建立：

> **Relevant Dependency Closure**

从任务所在位置向外扩展。

至少检查：

```text
Caller
  ↓
Target function
  ↓
Callee
  ↓
Callback
  ↓
State / data structure
  ↓
Configuration
  ↓
Test
```

必要时继续向外扩展。

---

## 5.1 Caller analysis

修改函数前搜索所有调用者。

例如：

```bash
rg "foo_init\(" .
```

确认：

* 谁调用它？
* 调用次数？
* 调用时机？
* 调用上下文？
* 是否允许失败？
* 返回值如何处理？
* 调用前需要什么状态？

---

## 5.2 Callee analysis

不要只阅读目标函数。

如果：

```c
foo()
{
    bar();
    baz();
}
```

而修改依赖 `bar()` 或 `baz()` 的行为，就必须检查对应实现。

特别关注：

```text
allocation
locking
sleeping
reference counting
ownership
error handling
side effects
callbacks
```

---

## 5.3 Callback analysis

对于事件驱动系统，callback 往往比当前函数更加重要。

检查：

```text
IRQ handler
worker
timer
completion
callback
probe
remove
release
close
open
read
write
ioctl
poll
async callback
USB completion
DMA callback
```

确认：

```text
Who registers it?
Who calls it?
Under which context?
What state does it access?
Who owns that state?
What happens after teardown?
```

---

# 6. Phase 4 — Understand Data and State Flow

代码理解不能只停留在函数层面。

必须识别重要：

```text
data structures
state machines
ownership
resource lifetime
buffer lifetime
thread/IRQ context
configuration state
```

---

## 6.1 Data flow

尽可能建立：

```text
Input
  ↓
Parsing
  ↓
Validation
  ↓
Transformation
  ↓
Storage
  ↓
Processing
  ↓
Output
```

例如：

```text
userspace framebuffer
    ↓
fbdev/DRM
    ↓
dirty region
    ↓
pixel conversion
    ↓
USB buffer
    ↓
URB
    ↓
USB device
```

不要仅根据函数名称猜测数据流。

---

## 6.2 State flow

如果组件存在状态机，应明确：

```text
Created
  ↓
Initialized
  ↓
Registered
  ↓
Active
  ↓
Stopping
  ↓
Released
```

确认：

* 哪个函数改变状态？
* 哪个字段表示状态？
* 哪些操作只允许在特定状态执行？
* teardown 是否可能与 callback 并发？
* error path 是否回滚状态？

---

# 7. Phase 5 — Understand Ownership and Lifetime

这是系统工程中最容易因为局部阅读而出错的部分之一。

对重要资源必须确认：

```text
Who creates it?
Who owns it?
Who references it?
Who releases it?
When can it disappear?
Can another thread access it?
Can a callback outlive it?
```

尤其检查：

```text
memory
buffers
URBs
DMA mappings
file descriptors
devices
clocks
regulators
GPIO
interrupts
workqueues
timers
threads
reference counts
locks
```

如果无法回答资源生命周期：

> 不应贸然修改相关代码。

---

# 8. Phase 6 — Understand Configuration and External Context

很多工程行为并不完全存在于 `.c` / `.cpp` 文件中。

必须根据项目类型检查：

```text
Kconfig
Kbuild
Makefile
CMake
Device Tree
defconfig
environment variables
configuration files
generated headers
compiler flags
linker scripts
board definitions
USB descriptors
protocol definitions
```

特别是 embedded/Linux 项目。

例如：

```text
driver.c
    ↓
Kconfig
    ↓
Makefile/Kbuild
    ↓
Device Tree
    ↓
hardware resource
```

不能只阅读 `driver.c` 就认为已经理解驱动。

---

# 9. Phase 7 — Search for Existing Patterns

在写新代码之前，寻找 repository 中已经存在的类似实现。

优先搜索：

```text
same API
same subsystem
same error handling
same resource lifecycle
same callback pattern
same synchronization pattern
same hardware abstraction
same test pattern
```

例如不要直接设计新的：

```c
foo_cleanup()
```

先搜索项目是否已经存在：

```text
bar_cleanup()
baz_release()
xxx_remove()
```

并观察项目惯用模式。

原则：

> **Existing pattern > invented pattern**

---

# 10. Phase 8 — Tests and Validation Infrastructure

开始实现前必须寻找已有测试。

检查：

```text
tests/
test/
testsuite/
examples/
selftests/
integration tests
scripts/
CI
benchmark
hardware tests
```

同时确认：

```text
How is this component tested?
What is the normal build command?
What is the normal test command?
What hardware is required?
What behavior is considered success?
```

如果没有自动化测试，也应该寻找：

```text
manual test procedure
example application
debug tool
CLI
hardware validation script
```

---

# 11. Phase 9 — Create an Evidence-Based Implementation Model

在修改代码前，应能够用简洁语言解释：

```text
1. Current behavior
2. Relevant execution path
3. Relevant data flow
4. Relevant state/lifecycle
5. Relevant configuration
6. Existing implementation pattern
7. Required change
8. Validation method
9. Remaining uncertainties
```

例如：

```text
Current behavior:
USB completion callback copies status into device state.

Execution path:
probe()
 → submit_urb()
 → completion()
 → worker()

Data flow:
framebuffer
 → staging buffer
 → URB

Required change:
Avoid resubmitting URB after device removal.

Relevant lifecycle:
remove()
 → kill_urb()
 → release buffer

Existing pattern:
Another USB path already uses the same teardown ordering.

Validation:
build module + unload/reload + stress disconnect.
```

如果无法做到这种程度，说明探索还不够。

---

# 12. Exploration Gate

在开始修改代码之前，执行以下检查。

```text
[ ] I know what the project does.
[ ] I know how it is built.
[ ] I know where execution starts.
[ ] I identified the relevant subsystem.
[ ] I traced the relevant caller/callee path.
[ ] I inspected relevant callbacks.
[ ] I understand important data structures.
[ ] I understand relevant resource ownership.
[ ] I checked configuration/build dependencies.
[ ] I searched for existing implementation patterns.
[ ] I located relevant tests or validation methods.
[ ] I know what behavior must change.
[ ] I know what behavior must remain unchanged.
[ ] I identified remaining uncertainties.
```

如果关键项目仍然是：

```text
UNKNOWN
```

则继续探索，而不是开始编码。

---

# 13. Do Not Over-Explore

本 Skill 不要求：

> 阅读 repository 中每一个文件。

这同样是一种低效行为。

采用：

```text
Global Scan
    ↓
Task Localization
    ↓
Dependency Closure
    ↓
Evidence
    ↓
Implementation
```

而不是：

```text
Read everything
    ↓
Consume huge context
    ↓
Forget important details
```

---

## 13.1 Stop condition

当以下条件成立时，可以停止探索：

```text
The requested behavior is localized.
The execution path is understood.
Relevant callers/callees are understood.
Relevant state/lifetime is understood.
Relevant configuration is understood.
Existing patterns have been checked.
Validation method is known.
No important unknown remains.
```

这就是合理的探索终点。

---

# 14. Implementation Rules

完成 Exploration Gate 后才能修改代码。

---

## 14.1 Make the smallest justified change

不要因为理解了整个工程，就顺手进行大规模重构。

优先：

```text
smallest change
    ↓
preserve existing architecture
    ↓
reuse existing patterns
    ↓
validate
```

除非用户明确要求，否则不要把：

```text
bug fix
```

扩大成：

```text
bug fix + architecture rewrite + refactor
```

---

## 14.2 Do not invent abstractions prematurely

不要仅仅为了“代码看起来更优雅”就新增：

```text
framework
layer
helper
wrapper
manager
generic abstraction
```

除非现有架构已经证明它需要。

---

## 14.3 Preserve local conventions

遵循 repository 已有：

```text
naming
formatting
error handling
logging
locking
resource management
API style
test style
```

不要因为个人偏好替换项目已经稳定使用的模式。

---

# 15. Handling Contradictions

如果发现：

```text
README says A
code does B
test expects C
```

不要自行选择一个。

建立事实：

```text
Documentation: A
Implementation: B
Test: C
```

然后判断哪个才代表当前行为。

通常：

```text
actual code + passing tests
```

比过时文档更能代表当前实现。

但如果行为本身是 bug，则必须明确区分：

```text
Current behavior
Expected behavior
```

不要把两者混为一谈。

---

# 16. Handling New Information

如果编码过程中发现原来的模型错误：

```text
Old model
    ↓
New evidence
    ↓
Contradiction
```

必须：

```text
STOP IMPLEMENTATION
        ↓
RETURN TO EXPLORATION
        ↓
UPDATE MODEL
        ↓
RE-EVALUATE CHANGE
        ↓
CONTINUE
```

禁止通过连续打补丁来掩盖错误的初始理解。

---

# 17. Avoid Context-Driven Hallucination

Agent 在长代码任务中容易出现：

```text
看到 A
↓
联想到 B
↓
假设 C
↓
开始修改 D
```

这不是可靠的软件工程流程。

对于关键结论，应尽可能回答：

```text
Where is the evidence?
```

例如：

```text
Claim:
This callback runs in process context.

Evidence:
worker thread invokes callback at foo.c:123.
```

而不是：

```text
This callback probably runs in process context.
```

---

# 18. Comments and Documentation

探索阶段产生的理解不应该自动变成大量代码注释。

不要：

```c
/*
 * This function is responsible for...
 *
 * The reason we do this is...
 *
 * Historically...
 *
 * According to...
 *
 * In some cases...
 */
```

如果代码本身已经足够清楚，不增加注释。

只有以下情况值得写注释：

```text
non-obvious invariant
hardware limitation
lifetime constraint
ordering requirement
workaround
subtle concurrency rule
external protocol requirement
```

注释应该解释：

> **Why**

而不是重新描述：

> **What the code obviously does**

---

# 19. Exploration Output Should Be Compact

不要把 repository exploration 变成一篇巨大的分析报告。

推荐格式：

```text
## Repository model

Project:
Build:
Entry:
Relevant subsystem:

## Execution path

A
 → B
 → C
 → D

## Data/state flow

A
 → B
 → C

## Important constraints

- ...
- ...
- ...

## Existing pattern

- ...

## Validation

- ...

## Remaining uncertainty

- ...
```

目标是：

> **足够支持正确实现，而不是生成另一份完整知识库。**

---

# 20. Anti-Patterns

以下行为视为探索失败。

### 20.1 Read one file and start coding

```text
open target.c
↓
find function
↓
edit
```

禁止。

---

### 20.2 Guess from symbol names

```text
foo_init()
```

不能推断：

```text
foo_init() only initializes memory
```

必须阅读定义和调用路径。

---

### 20.3 Guess from framework conventions

例如：

```text
Linux driver generally does X
```

不能替代：

```text
this driver's actual behavior
```

---

### 20.4 Ignore build/configuration

不要只修改：

```text
.c
```

却不检查：

```text
Kconfig
Kbuild
CMake
Device Tree
config
headers
```

---

### 20.5 Ignore teardown

任何涉及资源、异步执行、线程、IRQ、DMA、USB、network 或 hardware 的修改，都必须检查 teardown。

---

### 20.6 Patch until compilation succeeds

```text
compile error
↓
patch
↓
compile
↓
another error
↓
patch
```

编译成功不是架构理解正确的证明。

---

### 20.7 Treat successful runtime behavior as proof

```text
It runs
```

不等于：

```text
It is correct
```

还必须确认：

```text
lifetime
error path
concurrency
cleanup
boundary conditions
repeatability
```

---

# 21. Special Rules for Embedded/Linux Projects

对于：

```text
Linux kernel
Linux driver
BSP
Device Tree
Buildroot
Yocto
firmware
RTOS
MCU
USB
SPI
I2C
UART
DMA
DRM
V4L2
IIO
```

应额外检查：

```text
hardware topology
device tree
clock/reset
power
regulator
GPIO
interrupt
DMA
bus relationship
probe/remove lifecycle
runtime PM
suspend/resume
buffer ownership
userspace interface
```

特别注意：

```text
hardware behavior ≠ source-code behavior
```

如果上游有**官方例子/参考实现**（厂商 SDK 的 demo、上游 in-tree 驱动、`Documentation/` 里的示例），
**先跑通它**再写自己的代码 —— 见 §1.5。它是把 `driver` / `protocol` / `hardware datasheet` /
`firmware` / `device tree` 五件事一次对齐的最短路径，也是唯一能把"我的代码不对"与"硬件/配置不对"
分开的做法。

如果代码行为依赖硬件协议，应同时确认：

```text
driver
protocol
hardware datasheet
firmware
device tree
```

---

# 22. Special Rules for Existing Mature Projects

如果 repository 已经存在大量代码，不要默认：

```text
old code = bad code
```

首先理解：

```text
Why is it structured this way?
```

可能存在：

```text
hardware limitation
ABI compatibility
backward compatibility
performance constraint
kernel API constraint
legacy protocol
deployment constraint
```

只有理解原因后，才能判断是否应该改变。

---

# 23. Git History as Evidence

对于难以理解的代码，可以检查 Git history。

例如：

```bash
git log -- path/to/file
git blame path/to/file
git log -S 'symbol'
git log -G 'pattern'
```

重点寻找：

```text
Why was this code introduced?
What bug did it fix?
What constraint caused this design?
Was this workaround intentional?
```

Git history 是辅助证据，不是必须阅读所有历史。

---

# 24. External Documentation

如果 repository 内部证据不足，可以查询：

```text
official documentation
kernel documentation
vendor documentation
protocol specification
hardware datasheet
upstream implementation
```

优先：

```text
authoritative source
```

而不是随机博客或搜索结果。

外部知识不能覆盖 repository 的实际行为，除非明确确认 repository 实现应该遵循该规范。

---

# 25. Final Verification

修改完成后重新检查：

```text
[ ] Change matches the original requirement.
[ ] No unrelated behavior was changed.
[ ] Existing patterns are preserved.
[ ] Error paths remain valid.
[ ] Resource lifetime remains valid.
[ ] Concurrency assumptions remain valid.
[ ] Build configuration remains correct.
[ ] Relevant tests were executed.
[ ] New behavior was actually verified.
```

如果测试失败：

```text
Do not immediately patch the failure.

First determine:
Is the implementation wrong?
Is the test wrong?
Is the environment wrong?
Is the original assumption wrong?
```

---

# 26. Compact Agent Protocol

对于日常 Agent 工作，可以将整个 Skill 压缩成以下协议：

```text
ENTER UNKNOWN REPOSITORY

1. Scan repository structure.
2. Identify build system.
3. Identify entry points.
4. Locate the relevant subsystem.
5. Trace callers, callees and callbacks.
6. Understand data flow and state flow.
7. Understand ownership and lifetime.
8. Inspect configuration/build dependencies.
9. Search for existing implementation patterns.
10. Locate tests and validation methods.
11. Record remaining unknowns.
12. Only then modify code.

During implementation:

13. Prefer repository evidence over assumptions.
14. Reuse existing patterns.
15. Make the smallest justified change.
16. If new evidence contradicts the model, stop and re-explore.
17. Validate behavior, not merely compilation.
18. Do not turn exploration into unnecessary documentation.

CORE RULE:

    Understand the system before changing the system.

    Evidence > assumption.
    Existing pattern > invented pattern.
    Actual behavior > guessed behavior.
    Small justified change > speculative redesign.
```

# 27. Definition of Done

Repository exploration is complete when the Agent can explain, without guessing:

```text
What does this project do?

How is it built?

Where does execution begin?

Which subsystem implements the requested behavior?

How does control flow through that subsystem?

How does data flow through it?

What state and resources are involved?

Who owns those resources?

What configuration or hardware dependencies exist?

What existing implementation pattern should be followed?

How will the change be validated?

What assumptions remain uncertain?
```

如果这些问题无法回答，不应认为已经完成 Repository Exploration。
