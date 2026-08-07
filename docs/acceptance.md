# 双向 Heartbeat 验收

## 本地静态验收

以下检查应在每次修改后执行：

```sh
# 禁止回读、Future 型远端写入和 NMT 状态同步
grep -R "AsyncRead\|AsyncWrite\|master\.Read\|OnState\|waitForNmtState" \
    src include

# 必须存在的 callback、远端 SDO 写入和流程
grep -R "OnBoot\|OnHeartbeat\|OnEmcy\|SubmitWrite\|heartbeatProcess" \
    src include
```

第一组预期无结果；第二组必须命中 `nmt_heartbeat` 模块和 `main.cpp` 调用点。

配置检查：

```sh
grep -A4 '^\[1016Value\]' config/generated/master.dcf
grep -A4 '^\[1F81Value\]' config/generated/master.dcf
grep -n 'mcu_node_1.bin' config/generated/master.dcf
```

`mcu_node_1.bin` 必须包含且只包含：

```text
0x1016:01 = DC 05 7F 00
0x1017:00 = F4 01
```

Shell 和 Host 编译检查：

```sh
sh -n deploy/run.sh

c++ -std=c++14 -Wall -Wextra -Wpedantic -Wconversion \
    -Wsign-conversion -Wshadow -Werror=return-type \
    -Iinclude -I<lely-include> -I<third-party-spdlog-include> \
    -c src/main.cpp src/nmt_heartbeat.cpp src/shutdown_process.cpp
```

Host 对象编译只验证语法、模板实例化和工程内部接口，不代表 Yocto/aarch64 完整链接或目标板运行通过。

## 目标板验收步骤

同时启动主站日志和 CAN 抓包：

```sh
candump -t A -x can1
cmake --build build --target download
```

### 1. Boot 和正常双向 Heartbeat

通过条件：

- Boot callback 为 `status=success` 或 `status=L`；
- 不出现 Boot 状态 `'J'` 或 `'K'`；
- 总线持续存在 `0x77F`；
- 总线持续存在 `0x701`；
- 主站未报告节点 1 Heartbeat timeout。

Boot 成功后不再通过 SDO 回读 `0x1016/0x1017`。

### 2. 主站停止 Heartbeat

预期日志：

```text
Master producer heartbeat stopped
EMCY callback: node=1 code=0x8130 ...
Remote node detected master heartbeat timeout
```

通过条件：本地 `0x1017:00` 写 0 无错误，并在 `CANOPEN_WAIT_TIMEOUT_MS` 内收到节点 1 的 `0x8130` EMCY。不检查节点 1 的具体 NMT 状态。

### 3. 主站恢复 Heartbeat

预期日志：

```text
Master producer heartbeat restored
EMCY callback: node=1 code=0x0000 ...
Remote heartbeat error cleared
NMT Start sent to node 1
```

通过条件：本地 `0x1017:00` 恢复为 500 无错误，收到 EMCY reset，然后发送 NMT Start。不等待状态 callback。

### 4. 主站通过 SDO 停止从机 Heartbeat

第一阶段完成后，主站临时关闭节点 1 的自动 NMT error reset，并通过 `SubmitWrite()` 写：

```text
node 1 0x1017:00 = 0
```

预期抓包包含 `0x601/0x581` SDO request/response，约 1500 ms 后预期日志：

```text
Slave producer heartbeat stopped through SDO
Remote heartbeat timeout: node=1
Master detected slave heartbeat timeout
```

通过条件：远端 SDO 写成功，`OnHeartbeat(node=1, true)` 在 `CANOPEN_WAIT_TIMEOUT_MS` 内发生，且测试窗口内不因该 synthetic timeout 自动发送 NMT Reset Node。

### 5. 主站通过 SDO 恢复从机 Heartbeat

主站通过 `SubmitWrite()` 写：

```text
node 1 0x1017:00 = 500
```

预期：

```text
Slave producer heartbeat restored through SDO
Remote heartbeat recovered: node=1
Master detected slave heartbeat recovery
```

通过条件：远端 SDO 写成功，`0x701` 恢复 500 ms 周期，并收到 `OnHeartbeat(node=1, false)`。随后本地 `0x1F81:01` 必须恢复为 `0x00000005`。

### 6. Final Reset

按 `Ctrl+C` 后预期：

```text
Final Reset Communication sent to node 1
Boot callback: node=1 ...
Final Reset Communication completed
```

随后 event loop 退出，工作线程完成 `join()`，进程返回。若未收到退出信号而 event loop 提前停止，进程必须记录错误并返回失败。主站退出后节点产生新的 Heartbeat Consumer EMCY，不作为本轮失败。

## 验收矩阵

| 编号 | 项目 | 通过判据 |
| ---: | --- | --- |
| 1 | 配置生成 | 生成节点 1 的 `0x1016/0x1017` 写请求 |
| 2 | 部署文件 | 目标 `config/` 包含 `mcu_node_1.bin` |
| 3 | Boot | 状态 0 或 `L`，无 `'J'`/`'K'` |
| 4 | 正常 Heartbeat | 总线持续存在 `0x77F` 和 `0x701` |
| 5 | 无回读 | 源码无 `AsyncRead()` 和 `master.Read()` |
| 6 | 无 Future 型远端写入 | 源码无 `AsyncWrite()` |
| 7 | 无 NMT 状态判据 | 源码无 `OnState()` 和状态等待 |
| 8 | 主站中断 | 节点 1 上报 `0x8130` |
| 9 | 主站恢复 | 节点 1 上报 `0x0000` |
| 10 | NMT 恢复 | 主站发送 NMT Start，不等待状态 |
| 11 | 从机中断 | SDO 写 `0x1017=0` 后 `OnHeartbeat(..., true)` |
| 12 | 从机恢复 | SDO 写 `0x1017=500` 后 `OnHeartbeat(..., false)` |
| 13 | Assignment 恢复 | 自动测试后本地 `0x1F81:01=0x00000005` |
| 14 | 节点重启 | Lely 自动重新执行 concise DCF |
| 15 | 正常退出 | Final Reset Communication 保留 |
| 16 | 完整验证 | 交叉构建、目标抓包和本地多轮复审完成 |
