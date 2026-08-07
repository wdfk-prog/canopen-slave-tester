# 双向 Heartbeat 专项设计

## 设计边界

本轮只验证三条链路：正常双向 Heartbeat、主站 Producer Heartbeat 中断与恢复、从机 Producer Heartbeat 的自动 SDO 中断与恢复。

明确不执行：

- `AsyncRead()` 或 `master.Read()` 回读；
- `AsyncWrite()` 配置节点 1 Heartbeat；
- 独立 SDO 功能测试；
- `OnState()`、NMT 状态等待或状态断言；
- 独立 EMCY 模块；
- 人工断线作为 Heartbeat 通过判据。

自动测试允许 `SubmitWrite()` 仅对节点 1 的 `0x1017:00` 临时写 0/500，用于制造和恢复从机 Producer Heartbeat 中断；不回读对象字典。

## 文件职责

```text
src/main.cpp
├─ 创建并持有全部 Lely 和 SocketCAN 对象
├─ 注册 CAN、Boot、Heartbeat 和 EMCY callback
├─ 启动 Loop::run() 工作线程
├─ Reset 并等待 Boot
├─ 调用 heartbeatProcess() 完成双向自动 Heartbeat 测试
├─ 保持运行直到 Ctrl+C
├─ 调用 finalResetProcess()
└─ shutdown Context 并 join 线程

src/nmt_heartbeat.cpp
├─ Boot callback 和等待状态
├─ OnHeartbeat 日志及 timeout/recovery 等待状态
├─ OnEmcy 日志及自动流程私有等待状态
├─ 主站 Producer Heartbeat 中断/恢复测试
└─ 通过 SDO 中断/恢复从机 Producer Heartbeat

src/shutdown_process.cpp
└─ Reset Communication 并等待 Boot callback
```

不创建 `BasicDriver`。节点 1 的 Boot 和配置由 `AsyncMaster` 根据 `master.dcf` 完成。

## 启动和 Boot

启动顺序必须保持：

```text
注册 callback
→ 启动 event loop
→ prepareBootWait()
→ master.Reset()
→ waitForBootCompletion()
```

Boot 成功判据：

- `status == 0`；
- `status == 'L'`，表示节点在 Boot 流程开始时已处于 Operational。

其他状态或等待超时均阻止自动 Heartbeat 测试。Boot callback 同时用于运行期节点重启日志和 Final Reset 完成通知。应用不维护 runtime Boot pending，也不在 callback 中提交 SDO。

## 自动测试同步

EMCY 等待状态仅存在于 `nmt_heartbeat.cpp` 内部：

```text
expected EMCY code
received flag
mutex
condition variable
```

停止主站 Heartbeat 前先选择 `0x8130`；恢复主站 Heartbeat 前先选择 `0x0000`，避免事件在写操作完成后快速到达而丢失。

自动流程：

```text
prepare 0x8130 wait
→ master.Write<uint16_t>(0x1017:00, 0)
→ wait 0x8130
→ prepare 0x0000 wait
→ master.Write<uint16_t>(0x1017:00, 500)
→ wait 0x0000
→ master.Command(NMT Start, node 1)
```

本地 OD 写入只检查 `std::error_code`。不执行回读。若 `0x8130` 等待失败，仍恢复主站 Producer Heartbeat，防止测试失败后主站持续离线；函数最终返回失败。恢复写入或 EMCY reset 等待失败时，不发送 NMT Start。

## 从机 Producer Heartbeat 自动测试

Lely NMT master 在可选从机 Heartbeat timeout 时默认会发送 NMT Reset Node。为了让恢复动作明确由测试阶段的远端 SDO 写入完成，测试窗口临时把主站本地 `0x1F81:01` 从 `0x00000005` 写为 `0`，使该 synthetic timeout 不触发默认 reset；Heartbeat consumer 本身保持工作。

```text
prepare OnHeartbeat(true) wait
→ local 0x1F81:01 = 0
→ SubmitWrite(node 1, 0x1017:00, 0)
→ wait OnHeartbeat(node=1, true)
→ prepare OnHeartbeat(false) wait
→ SubmitWrite(node 1, 0x1017:00, 500)
→ wait OnHeartbeat(node=1, false)
→ local 0x1F81:01 = 0x00000005
```

`SubmitWrite()` 的 SDO completion 通过独立 condition variable 等待；远端 SDO 返回错误或超时均计为测试失败。测试结束后恢复 `0x1F81:01`，因此后续运行和 Final Reset 继续使用原来的 Boot assignment。节点重新启动时，Lely 仍根据 `mcu_node_1.bin` 自动配置 Heartbeat。

## 退出

收到 `SIGINT` 或 `SIGTERM` 后：

```text
finalResetProcess(master)
→ Reset Communication node 1
→ wait Boot callback
→ Context::shutdown()
→ join event-loop thread
```

由于 `boot:true`，Final Reset 会再次执行节点配置。主站随后退出导致节点 1 检测到主站 Heartbeat 丢失，属于真实离线行为，不计入自动测试结果。

## 兼容性和影响范围

- `heartbeatProcess()` 接口变为仅接收 `AsyncMaster&`；
- `finalResetProcess(AsyncMaster&)` 保持不变；
- 不修改从机程序、CANopenNode、RT-Thread CAN 驱动或 Lely；
- 不增加公共 EMCY API；
- 不改变 CAN 接口、Node-ID、目标部署目录或 CMake 目标名称；
- 新增部署文件 `config/mcu_node_1.bin`，并保证 `master.dcf` 中的绝对路径与目标布局一致。
