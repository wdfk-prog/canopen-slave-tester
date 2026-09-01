[English](../en/cia303-led-test.md)

# CiA 303-3 LED 手工验证

仓库保留了一份基于 `coctl` 的 CiA 303-3 LED 详细手工验证记录。该测试属于人工/HIL 流程，不进入 `canopen_master` 自动协议验证表。

## 目的

通过 Host 工具构造预期的 NMT/通信状态，把 MCU 的 CANopen 通信/错误状态与 RUN/ERROR LED 表现进行对应验证。

## 建议记录的证据

每个手工 case 至少保留：

- 触发状态的 Host 命令；
- 对应 CAN trace；
- MCU NMT/通信/error state；
- 实际观察到的 RUN/ERROR LED 模式与时序；
- 进入下一个 case 前的恢复动作。

## 边界

LED 视觉行为属于真实硬件/HIL 结论。CI 交叉编译或源码静态检查不能验证 LED 时序或电气行为。

## 详细参考

完整旧版中文操作记录保留在 [`reference/CANopen_CiA303-3_LED_coctl_test.md`](reference/CANopen_CiA303-3_LED_coctl_test.md)。
