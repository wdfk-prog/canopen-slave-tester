[English](../en/testing.md)

# 测试与验证

## 证据模型

Host 测试主要组合三类证据：

1. Lely 标准 CANopen service 的完成结果和 callback；
2. 通过 SDO 回读 MCU Object Dictionary 状态；
3. 对 Lely 高层 service 未覆盖协议帧使用职责受限的 wire-level 注入/捕获。

某个流程返回 PASS，只能证明该流程实际实现的断言成立；它不会自动证明整段测试期间的目标时序裕量、总线健康、真实掉电行为或功能安全认证。

## 验证能力与 DUT 前置条件

| 能力 | 主要证据 | MCU/DUT 额外要求 |
| --- | --- | --- |
| Heartbeat | Boot/Heartbeat callback、EMCY/SDO | 已配置 Heartbeat producer/consumer |
| SDO object access | upload/download/read-back | 测试使用的可写用户 OD |
| SDO server block transfer | block upload/download、abort/recovery | MCU SDO server block 能力与测试对象 |
| Storage persistence | OD 命令、raw backup/CRC、reset/power-cycle 流程 | CANopenNode storage backend；破坏性模式可能要求人工操作 |
| MCU SDO client | MCU diagnostic 控制/状态 OD + Host SDO-server fixture | MCU SDO client 测试诊断对象 |
| PDO | Lely PDO callback/mapped value + SDO 回读 | 对应 PDO mapping |
| SYNC/synchronous PDO | Lely SYNC producer/callback 时序 + TPDO | SYNC consumer 与同步 TPDO |
| TIME consumer | TIME 注入 + MCU diagnostic OD | MCU 暴露 TIME diagnostic record |
| EMCY producer | Host EMCY observer + 0x1001/0x1003/0x1014/0x1015 | 流程要求的 configurable/history 能力 |
| EMCY consumer | Host local EMCY producer + MCU diagnostic OD | MCU EMCY consumer diagnostic record |
| GFC | MCU 0x1300/0x2302 + 固定 CAN-ID 0x001 fixture | CANopenNode GFC consumer/producer diagnostic |
| SRDO | MCU SRDO 控制/状态 + 成对 wire capture/fault case | 已配置测试使用的 CANopenNode SRDO profile |
| NMT Master behavior | Lely `BasicSlave` 状态 callback | MCU 作为 NMT Master 主动控制 Node 2 |

## 当前默认配置

当前 Master 自动流程中仅 SRDO 默认启用；Final Reset Communication 清理开启。其他协议实现保留在源码中，但必须修改对应编译开关后才会进入流程表。

如果需要同时启用多个流程，应先确认它们对 MCU 固件配置、临时 OD 值、Host role 和 cleanup 前提不存在冲突。

## Fail-fast 与 cleanup

Master role 按固定顺序执行已启用流程。任一流程失败后停止后续验证。每个流程负责恢复自己临时修改的对象；无法证明安全恢复时，应保持失败并明确环境 dirty，而不是继续宣称 PASS。

Final Reset Communication 是独立退出动作，由 `CANOPEN_ENABLE_FINAL_RESET_PROCESS` 控制。

## 仍需外部 HIL 的证据

不同流程的完整目标板验收可能还需要：

- `candump` 或独立 CAN trace；
- 确认控制器全程未进入 error-passive/bus-off；
- 真实 power-cycle 或 interrupted-write 人工动作；
- 目标端内存/资源监控；
- MCU 日志或 diagnostic OD snapshot；
- 实际总线/目标板时序测量。

TQ8MP CI/Release 交叉编译不提供这些 HIL 结论。

## 协议专项

- [NMT Master 验证](nmt-master-test.md)
- [EMCY Consumer 验证](emcy-consumer-test.md)
- [GFC 协议验证](gfc-test.md)
- [SRDO 协议验证](srdo-test.md)
- [CiA 303-3 LED 手工验证](cia303-led-test.md)

旧版的长篇测试记录保留在 reference 目录中，只作为补充资料；当前开关、构建和验证边界以维护中的中英文页面为准。
