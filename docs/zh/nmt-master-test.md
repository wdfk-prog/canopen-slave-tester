[English](../en/nmt-master-test.md)

# NMT Master 验证

NMT Master 验证会反转正常的 Host/DUT 关系。Linux 程序编译为 `CANOPEN_ROLE_SLAVE`，创建 Node-ID 2 的 Lely `BasicSlave`；被测 MCU 必须作为 NMT Master，主动驱动这个标准 CANopen peer 完成预期状态迁移。

## 启用方式

重新编译前配置：

```c
#define CANOPEN_ROLE CANOPEN_ROLE_SLAVE
#define CANOPEN_ENABLE_NMT_MASTER_PROCESS 1
```

验证开关位于 `include/nmt_master_process.h`，角色位于 `include/canopen_config.h`。

## Host 实际观察什么

软件 peer 注册 Lely NMT callback，并根据外部收到的 NMT 命令验证状态序列。DUT 必须主动发起测试；Linux peer 不通过 SDO 或通用 Raw CAN 注入伪造 MCU 的 NMT Master 行为。

当前实现同时使用软件 peer 的 Producer Heartbeat，使 MCU 能在测试序列中监督 Node 2。

## 证据边界

Lely `BasicSlave` callback 能证明 peer 协议栈已经接收 NMT 命令并进入对应状态迁移路径。完整目标板验收仍应关联：

- MCU 侧 NMT Master 操作/日志；
- Host callback 顺序与 timeout；
- 实际 NMT CAN 帧抓包；
- 最终 Operational 后的 Heartbeat 连续性。

## 详细参考

旧版完整中文设计/测试记录保留在 [`reference/CANopen_NMT_Master_Test.md`](reference/CANopen_NMT_Master_Test.md)。
