[English](../en/nmt-master-test.md)

# NMT Master 验证与 Slave peer profiles

Linux 程序使用 `CANOPEN_ROLE_SLAVE` 时可选择两种 Lely `BasicSlave` 测试夹具。之所以分开，是因为历史 CANopenNode NMT 测试要求固定命令序列，而 `lely-canopen-rtt` B4 Master 会执行自己的 NMT boot/reset 流程。

## lely-canopen-rtt B4 集成 profile

验证 `lely-canopen-rtt` PR #4 类 B4 Master 行为时使用：

```c
#define CANOPEN_ROLE CANOPEN_ROLE_SLAVE
#define CANOPEN_PEER_PROFILE CANOPEN_PEER_PROFILE_LELY_RTT_B4
#define CANOPEN_ENABLE_NMT_MASTER_PROCESS 0
```

该 profile 创建 Node 1，加载 `config/lely_rtt_node1.dcf`。其中 Identity、预定义 SDO 连接、1000 ms Producer Heartbeat、`0x2000/0x2001` 测试对象与 B4 Node1 夹具一致。

此模式只通过 `BasicSlave::OnCommand()` 被动记录 START、STOP、PREOP、RESET_NODE、RESET_COMM，不要求固定到达顺序，因此不会把 Lely NMT boot 自动产生的 Reset Communication 误判为 DUT 失败。

推荐把以下证据关联起来：

- MCU 侧 `co nmt ...`、`co node 1`、`co boot 1` 输出；
- Host 被动 NMT transition callback 日志；
- `candump` 实际 CAN 帧；
- SDO upload/download/abort，以及 Heartbeat 掉线/恢复。

确定性的 active-SDO cancellation 仍需要一个不会阻塞 Lely event loop 的 SDO response stall/fault-injection seam。本次 peer 不通过阻塞 `OnRead/OnWrite` 伪造，因为那会同时阻止 NMT reset/stop 被处理。

## 历史 CANopenNode 严格 NMT 序列

继续执行原 CANopenNode NMT Master 自动测试时使用：

```c
#define CANOPEN_ROLE CANOPEN_ROLE_SLAVE
#define CANOPEN_PEER_PROFILE CANOPEN_PEER_PROFILE_CANOPENNODE_NMT
#define CANOPEN_ENABLE_NMT_MASTER_PROCESS 1
```

该 profile 保持 Node 2、`config/project.eds`、500 ms Heartbeat 和原有固定 callback 序列。`nmtMasterProcess()` 会独占 `BasicSlave::OnCommand()` callback，因此被动 trace 会自动关闭。

## 证据边界

Lely `BasicSlave` callback 可以证明 peer 协议栈进入了对应 NMT 状态迁移路径，但它本身不能证明总线上一定收到了一帧匹配的 NMT command；wire-level 命令证据必须以 `candump` 为准。完整 HIL 仍必须结合 MCU 状态、Host 日志和 CAN 抓包。仅运行 Host peer 不能证明 RT-Thread owner-thread 生命周期、目标 CAN 时序或 shutdown race 已通过。

旧版完整设计/测试记录仍保留在 [`reference/CANopen_NMT_Master_Test.md`](reference/CANopen_NMT_Master_Test.md)。