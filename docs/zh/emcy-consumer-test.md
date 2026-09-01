[English](../en/emcy-consumer-test.md)

# EMCY Consumer 验证

该流程验证 MCU 作为 EMCY Consumer 的方向，与普通 MCU EMCY Producer 测试相反：

```text
Host local EMCY producer（Node 127）
    -> CAN EMCY frame
    -> MCU CANopenNode EMCY consumer
    -> MCU diagnostic OD 0x2301
    -> Host SDO 回读断言
```

## 前置条件

MCU 固件必须启用 EMCY Consumer，并提供项目诊断记录 `0x2301`。Host local EMCY producer 在测试开始前必须没有 active error，避免测试 cleanup 误删真实 Host fault。

## 覆盖内容

当前实现覆盖：

- 单次 EMCY 与 manufacturer-specific information；
- 连续不同 EMCY；
- duplicate EMCY，不假设 Host 去重；
- 标准 `0x0000` recovery；
- EMCY callback 后普通 SDO health；
- Reset Communication 后 callback/diagnostic rebind；
- count/snapshot 一致性和 cleanup ownership。

## Cleanup 规则

只有能证明属于本流程产生的 EMCY 才允许清理。如果 Host local EMCY stack 已经不再符合预期 test-owned error，流程必须拒绝清栈并保持失败，不能删除其他真实 fault。

## HIL 边界

自动 PASS 主要依赖 SDO diagnostic snapshot 和 Lely/Boot evidence。如果验收要求证明总线期间未进入 error-passive/bus-off，仍需独立 CAN state 日志或 `candump`。

## 详细参考

旧版完整记录保留在 [`reference/CANopen_EMCY_Consumer_Test.md`](reference/CANopen_EMCY_Consumer_Test.md)。
