# CANopenNode NMT Master 测试设计

## 1. 目的

本阶段验证 MCU 上 CANopenNode 的 NMT Master 命令产生和远端节点控制行为。

测试拓扑：

```text
STM32F407 / CANopenNode
Node-ID 1
NMT Master (DUT)
        |
        | NMT 0x000
        v
TQ8MP Linux / Lely BasicSlave
Node-ID 2
Test peer
```

与 A01～A06 不同，本阶段 Linux 端不是 CANopen Master。Linux 程序编译为 Lely `BasicSlave`，作为被 MCU 控制的真实 CANopen 从节点。

因此代码职责明确分为：

```text
CANOPEN_ROLE_MASTER
    -> AsyncMaster
    -> A01～A06

CANOPEN_ROLE_SLAVE
    -> BasicSlave Node 2
    -> nmtMasterProcess()
```

NMT Master 流程不进入 Master 角色的 Process 表。

## 2. 角色切换与构建方式

工程继续使用原有单一 `CMakeLists.txt`、单一 `src/*.cpp` 源码集合和单一可执行目标，不为 Slave 角色建立第二套 CMake 或框架。

角色直接由 `include/canopen_config.h` 中的一个宏选择：

```c
#define CANOPEN_ROLE_MASTER 1
#define CANOPEN_ROLE_SLAVE  2

#define CANOPEN_ROLE CANOPEN_ROLE_SLAVE  /* NMT Master HIL */
/* 或 */
#define CANOPEN_ROLE CANOPEN_ROLE_MASTER /* A01～A06 */
```

修改 `CANOPEN_ROLE` 后重新执行原有 CMake 构建即可。CMake 不负责角色选择，不新增第二个 target，也不改变 `src/*.cpp` source list。Master/Slave 业务实现分支仍只在 `main()` 最终入口选择处出现；`canopen_config.h` 继续负责角色值和非法值检查。当前 target 名保持 `canopen_master`，避免改变既有部署、debug target 和脚本接口。

## 3. 公共运行时

Master 和 Slave 复用完全相同的 Host runtime 初始化：

```text
spdlog
-> signal handler
-> IoGuard
-> Context
-> Poll
-> Loop
-> Executor
-> Timer(CLOCK_MONOTONIC)
-> CanController(can1)
-> bitrate check
-> CanChannel
```

这些对象只在 `main.cpp` 创建一次。

角色分支只负责创建协议对象：

```text
Master: AsyncMaster
Slave : BasicSlave
```

Event loop worker 也共用同一组 `startCanopenWorker()` / `stopCanopenWorker()` helper。

启动同步使用 `std::condition_variable::wait_for()`，不使用 1 ms sleep 轮询。

### Context shutdown

`context.shutdown()` 保留在统一的 `stopCanopenWorker()` 中。原因不是业务流程需要主动 reset Context，而是 `CanChannel`、`Timer` 等 Lely I/O service 仍挂在 Context 上。退出前 shutdown 用于取消 pending I/O，使 `loop.run()` 可以结束，然后再 `join()` event-loop thread，最后才销毁 role-owned Lely object 和 I/O 对象。

因此它属于公共资源生命周期清理，不属于 Master/Slave 协议行为。

## 4. Slave Peer 配置

Linux peer 固定：

```text
Node-ID = 2
CAN interface = can1
bitrate = 1 Mbit/s
```

使用现有：

```text
config/project.eds
```

`BasicSlave` 的 `dcf_bin` 参数继续传空字符串，不加载 concise DCF。`config/project.eds` 继续作为 MCU 提供的设备描述使用，Host 不为 software peer 修改其中的 NMT startup 对象，也不增加额外 `slave_peer.bin`。`project.eds` 中 `0x1017` 默认仍保持 `0`。Slave role 先执行正常 `BasicSlave::Reset()`，随后 `nmtMasterProcess()` 注册 `OnCommand()` observer，再通过 Lely 本地 `Write<uint16_t>()` 把 `0x1017` 设置为 `CANOPEN_PEER_HEARTBEAT_MS=500 ms`。

该 Heartbeat 是 MCU 自动 NMT Master 测试的上线与状态证据。若 Node 2 按当前 EDS startup 自动进入 Operational，MCU 在正式六步序列前先发送一次额外 `ENTER_PREOP`，等待新的 `702#7F` 后再开始 START。RESET_COMM/RESET_NODE 后 Lely 也可能按同一 startup 自动进入 Operational；Host 在 reset completion 的内部 PRE-OP callback 后恢复 `0x1017=500 ms`，MCU 收到后续 Operational heartbeat 时再次发送 fixture PREOP 归一化，然后继续下一条正式命令。

因此 Host 只提供被控 software peer、Heartbeat 和 callback 证据；PRE-OP 测试初态由 DUT 的 NMT Master 命令准备，不通过修改 MCU EDS 或访问 Lely protected NMT internals 获得。

## 5. NMT Master 测试流程

### 5.1 测试边界

Linux Slave 不负责向 MCU 发 NMT Command，也不通过 Raw SocketCAN 解析 `0x000`。

验证依据使用 Lely `BasicSlave::OnCommand()`：

```text
MCU CO_NMT_sendCommand()
        |
        v
CAN NMT command
        |
        v
Lely BasicSlave internal NMT state machine
        |
        v
OnCommand(command)
        |
        v
nmtMasterProcess() assertion
```

这能验证命令实际进入 CANopen Slave 的 NMT 状态机，而不是仅证明总线上出现两个字节。

### 5.2 MCU 侧自动测试契约

MCU 测试固件启用自动 NMT Master demo 后，不需要 FinSH/SDO/Raw CAN 触发。Host Slave 负责提供持续 Heartbeat 和 `OnCommand()` 验证，MCU 负责通过 Heartbeat Consumer 判断 Node 2 已上线并自动推进命令序列。

推荐 MCU 配置：

```text
PKG_CANOPENNODE_DEMO_NMT_MASTER_TEST=y
PKG_CANOPENNODE_DEMO_NMT_MASTER_TEST_TARGET_NODE_ID=2
PKG_CANOPENNODE_DEMO_NMT_MASTER_TEST_HB_TIMEOUT_MS=1500
PKG_CANOPENNODE_DEMO_NMT_MASTER_TEST_STATE_TIMEOUT_MS=3000
```

Host Node 2 完成 `BasicSlave::Reset()` 后先注册 `OnCommand()` observer，再把 Producer Heartbeat 配成 500 ms。MCU 通过 Heartbeat Consumer 发现 Node 2；如果首次有效状态不是 PRE-OP，则先发送一条 fixture-preparation `ENTER_PREOP` 并等待新的 PRE-OP heartbeat。该准备命令不计入正式六步序列，Host 单独消费其 callback。之后每条普通 NMT 命令都等待目标 state；RESET_COMM/RESET_NODE 先要求看到 Boot-up 导致的 consumer `UNKNOWN`，随后如果 peer 因 EDS startup 自动回到 Operational，MCU 再执行一次 PRE-OP 归一化。

因此 HIL 启动顺序不再要求“精确等 ready 日志后人工触发 MCU”。只要 Host Slave 已启动并持续发 Heartbeat，MCU 即使稍晚启动，也会自动完成 fixture 准备并执行正式序列：

```text
[fixture PREOP when needed]
START -> STOP -> PREOP -> RESET_COMM -> RESET_NODE -> START
```

Host 的 `OnCommand()` observer 会在启用 Producer Heartbeat 和输出 `NMT master validation ready` 前完成注册，因此 fixture PREOP 和正式命令都不会丢失。最后一条 START callback 通过后，Host 会确认 500 ms Producer Heartbeat 仍启用，并在两个完整 Heartbeat 周期内继续观察 callback FIFO；该窗口必须保持安静，出现任何额外 NMT callback 都判 FAIL。窗口结束后才宣布 Host 侧验证通过，从而给 MCU 最终 OPERATIONAL Heartbeat 断言提供生命周期保证。

### 5.3 Lely callback 期望序列

对于普通状态命令，Lely 从对应 NMT state entry 调用 `OnCommand()`：

```text
START       -> START
STOP        -> STOP
ENTER_PREOP -> ENTER_PREOP
```

Host 不修改 MCU 提供的 EDS，但会读取本地 `0x1F80` 的 NMT startup 值来确定 fixture 的确定性行为。bit 2 为 0 时 reset 后必然 auto-start；如果 `0x1F80` 不存在，按 Lely 的缺省 startup 值 0 处理，同样要求 auto-start。bit 2 为 1 时 peer 保持 PRE-OP。首次启动和每次 reset 后都按该已知 startup 契约验证，而不是靠等待窗口猜测 START 的来源。

Reset Communication 的必需 callback 为：

```text
RESET_COMM                 # MCU 正式命令
ENTER_PREOP                # Lely reset completion；Host 在这里恢复 0x1017
```

如果 startup bit 2 为 0，下一条正式命令前必须严格出现：

```text
START                      # fixture auto-start
ENTER_PREOP                # MCU fixture normalization
```

如果 startup bit 2 为 1，则这两个 callback 不应出现，Host 直接接受下一条正式 `RESET_NODE`。

Reset Node 的必需内部 callback 为：

```text
RESET_NODE                 # MCU 正式命令
RESET_COMM                 # Lely 内部 reset communication
ENTER_PREOP                # reset completion；Host 恢复 0x1017
```

RESET_NODE 后继续使用同一个 startup 契约：bit 2 为 0 时必须严格收到 `fixture START -> MCU ENTER_PREOP -> formal final START`；bit 2 为 1 时直接要求 formal final START。Host 不再把“等待两个 Heartbeat 周期没有收到 PREOP”当作 formal START 的正向证据，因此 DUT 在 fixture START 后停止运行会明确 FAIL。

因此 Host 始终严格断言六条正式命令、reset 必需内部 callback，以及由 `0x1F80` 确定存在的 fixture 分支。任一必需 callback 超时、callback FIFO overflow、fixture 分支缺失/顺序错误或出现其他命令立即 FAIL。

callback 侧使用固定长度 FIFO 保存到达顺序，而不是只保存“最后一条命令”。这是 Reset 流程必须的：`RESET_NODE -> RESET_COMM -> ENTER_PREOP` 可能在 event-loop 线程中连续发布，等待线程不保证能在每两个 callback 之间得到调度。

## 6. 为什么不使用 Raw CAN capture

本阶段的主要目标是验证 CANopenNode NMT Master 能实际控制一个标准 CANopen Slave。

Raw capture 只能证明：

```text
000#0102
```

曾经出现在总线上。

Lely Slave callback 则证明该 NMT 命令已经被 CANopen 协议栈接受并进入相应 NMT 状态迁移流程，因此作为 B04 主断言更合适。

辅助 `candump` 仍可用于调试，但不是 PASS 必需条件。

### 当前无法从 BasicSlave callback 区分的内容

`BasicSlave::OnCommand()` 只提供 NMT command specifier，不提供原始 NMT frame 的 target byte。因此 Slave 行为验证不能区分：

```text
START Node 2
```

和：

```text
Broadcast START Node 0
```

因为两者对 Node 2 的最终行为相同。

如果后续测试必须严格断言 `target=0` 与 `target=2` 的 wire encoding，再增加独立的总线级证据；当前不为了这一项重新引入 RawCanProbe。

## 7. 代码文件

```text
include/canopen_config.h
    角色选择、Node-ID、peer EDS、公共 runtime 参数

include/nmt_master_process.h
    NMT Master 流程开关、单步 timeout、公开入口

src/nmt_master_process.cpp
    Lely BasicSlave OnCommand observer
    固定 callback FIFO
    阻塞超时等待
    command sequence assertion

src/main.cpp
    公共 Host runtime
    Master/Slave role implementation
    唯一角色条件编译入口

deploy/run.sh
    Master/Slave 共用部署脚本
```

## 8. 验收

Host 静态检查：

```sh
grep -R "CANOPEN_ROLE" src include
```

预期 Master/Slave 业务实现分支只存在于 `src/main.cpp` 最终入口；角色选择只通过 `include/canopen_config.h` 中的 `CANOPEN_ROLE` 宏完成。

确认 Master 流程表没有 NMT Master 测试：

```sh
grep -n "g_canopen_processes\|nmtMasterProcess" src/main.cpp
```

要求：

- `g_canopen_processes` 只注册 A01～A06；
- `nmtMasterProcess()` 仅由 `runCanopenSlave()` 调用。

Slave 目标板验证：

1. 将 `include/canopen_config.h` 中的 `CANOPEN_ROLE` 设为 `CANOPEN_ROLE_SLAVE`；
2. 完整交叉构建并部署；
3. 启动 Linux peer；
4. 确认 Node 2 完成 `BasicSlave::Reset()` 并进入 NMT 初始化流程；
5. 启动 MCU NMT Master 测试序列；
6. 9 个 callback 全部按顺序 PASS；
7. CAN controller 不进入 passive/bus-off；
8. 进程最终返回 0。

Master 回归：

1. 将 `include/canopen_config.h` 中的 `CANOPEN_ROLE` 设为 `CANOPEN_ROLE_MASTER`；
2. A01～A06 的原有注册顺序和行为保持不变；
3. NMT Master 流程不应出现在 Master 自动测试日志中。

## 9. 当前未验证边界

当前代码侧验证不等价于目标板 B04 PASS。最终仍需要：

- MCU 实际启用 `CO_CONFIG_NMT_MASTER`；
- MCU 启用自动 NMT Master demo，并通过 Heartbeat Consumer 自动启动测试；
- TQ8MP + `can1` 实际运行 Lely BasicSlave Node 2；
- 实际 CAN 总线验证；
- MCU Reset Communication/Reset Node 后的完整实机时序确认。
