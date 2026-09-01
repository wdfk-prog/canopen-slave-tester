[English](../en/gfc-test.md)

# GFC 协议验证

GFC 流程验证 CANopenNode Global Fail-safe Command。当前项目使用的 Lely 高层 API 没有直接提供 GFC service，因此 Host 仍通过正常 `AsyncMaster` channel 做 CANopen 控制和 SDO，同时单独使用第二条 Lely `CanChannel` 处理固定 CAN-ID `0x001` 的 wire evidence。

## MCU 诊断契约

流程通过普通 SDO 访问 MCU 的 GFC 配置/诊断对象（`0x1300`、`0x2302`）观察 consumer/producer 状态。

## 覆盖内容

- preflight 与 baseline；
- enable 状态下 valid consumer delivery；
- disabled consumer gate；
- invalid DLC rejection；
- producer wire format（`0x001`、DLC 0）；
- 连续 GFC 计数；
- Reset Communication 后 callback rebind；
- GFC 后普通 SDO health；
- 测试临时参数恢复。

## 回归边界

流程内部只执行自己的普通 SDO smoke。如果要声明 GFC 不影响其他所有 CANopen 功能，必须在 GFC 后按项目选定的更大回归 profile 重新执行对应协议流程，不能由 GFC 单流程 PASS 推导。

该流程仅验证协议功能，不声明 SIL、PL、WCET/安全时间预算、真实执行器 safe state 或系统级功能安全认证。

## 详细参考

旧版完整记录保留在 [`reference/CANopen_GFC_Test.md`](reference/CANopen_GFC_Test.md)。
