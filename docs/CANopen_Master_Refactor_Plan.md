# CANopen Master Boot Callback 同步实施计划

> 文档状态：当前实施基线
> 适用工程：`canopen-master`
> 目标平台：TQ8MP Linux/aarch64 + Lely CANopen，RT-Thread + CANopenNode MCU 从机

## 1. 目标与范围

工程保持按 CANopen 协议域组织。由于 `config/master.yml` 继续使用 `boot: false`，应用不再以 `BasicDriver::IsReady()` 作为启动或 Reset 完成判据，而是使用 Lely `OnBoot` callback 跨线程通知主线程。

保持不变：

- `main()` 创建并持有全部 Lely 对象；
- 独立 CANopen 线程只执行 `Loop::run()`；
- `AsyncMaster`、`BasicDriver`、局部 SDO Future 和异步 spdlog 保留；
- NMT、SDO 与关闭流程仍按协议域拆分；
- `boot: false`、部署目录和 CMake target 保持不变；
- 不恢复统一 callback 文件、Runtime、sequence 或 generation counter。

本轮修改：

- Boot callback 和同步状态归入 `nmt_heartbeat.cpp`；
- 启动 Reset 与最终 Reset 均使用 Boot callback；
- 删除全部 `IsReady()` 轮询和状态轮询宏；
- 更新公共接口、Doxygen 和项目文档。

## 2. 文件职责

### `src/main.cpp`

负责资源生命周期、callback 注册、event-loop 线程、流程编排和退出。启动顺序为：

```text
prepareBootWait()
→ master.Reset()
→ waitForBootCompletion()
→ nmtHeartbeatProcess()
→ sdoProcess()
```

启动 Boot 失败后不执行依赖流程，也不在退出时发送最终 Reset。

### `src/nmt_heartbeat.cpp`

拥有两套独立同步通道：

- Boot：`OnBoot`、Boot mutex/condition、状态、错误码和诊断文本；
- NMT：`OnState`、NMT mutex/condition 和目标状态。

每次 Reset 前必须先清除 Boot 状态。Boot callback 只接受 `CANOPEN_SLAVE_NODE_ID`，复制结果、通知主线程并记录诊断，不执行协议流程。

Boot 成功判据：

- `status == 0`：成功；
- `status == 'L'`：从机初始处于 Operational，记录 warning 后允许继续；
- 其他状态或超时：失败。

### `src/shutdown_process.cpp`

负责：

```text
prepareBootWait()
→ 定向 Reset Communication
→ waitForBootCompletion()
```

不再接收 `BasicDriver&`，不访问 Context、Loop、Channel 或线程对象。

## 3. 并发约束

Reset 与 Boot 等待严格串行：

```text
prepareBootWait()
→ 发送 Reset
→ 等待 callback
```

禁止先发送 Reset 再清除状态，否则快速到达的 callback 可能丢失。

启动 Boot 超时后：

- 不执行 NMT/SDO；
- 不执行最终 Reset；
- event loop 保持运行，等待人工退出和现场调试；
- 迟到 callback 只更新模块状态，不驱动后续流程。

因此当前单节点、串行 Reset 模型不需要 sequence counter。

## 4. 配置

`include/canopen_config.h` 保留：

```c
#define CANOPEN_BOOT_TIMEOUT_MS 5000
```

该宏表示等待目标节点 Boot callback 的最大时间。

删除：

```c
CANOPEN_STATUS_POLL_INTERVAL_MS
```

`config/master.yml` 保持：

```yaml
boot: false
mandatory: false
reset_communication: true
```

## 5. 验证

静态检查：

```sh
grep -R "IsReady()\|waitForDriverReady\|waitForDriverNotReady" src include
grep -R "g_boot_sequence\|boot_generation" src include
```

预期均无结果。

Boot 接口检查：

```sh
grep -R "prepareBootWait\|waitForBootCompletion\|g_boot_condition" src include
```

预期只出现在 NMT 模块、启动流程和最终 Reset 流程。

目标板启动预期：

```text
CANopen event loop started
Boot callback: node=1 ... status=success
Remote node 1 completed startup Boot
NMT and heartbeat process started
```

不应再出现：

```text
BasicDriver did not become ready during startup
```

最终 Reset 预期：

```text
Final reset communication process started
Boot callback: node=1 ... status=success
Final reset communication process completed
```

## 6. 验收标准

1. `boot: false` 保持不变；
2. 源码不调用 `BasicDriver::IsReady()`；
3. Boot callback 位于 `nmt_heartbeat.cpp`；
4. Boot 与 NMT 使用独立 mutex/condition；
5. 每次 Reset 前先调用 `prepareBootWait()`；
6. callback 只接受目标 Node-ID；
7. Boot 状态 0 和 `'L'` 可继续，其他状态失败；
8. 不存在 Boot sequence 或 generation；
9. 启动 Boot 失败时不执行最终 Reset；
10. `finalResetProcess()` 只接收 `AsyncMaster&`；
11. SDO 与 event-loop 线程模型保持不变；
12. CMake 下载、部署和 Debug target 保持兼容。
