# A01-A03 自动测试验收

## 本地静态验收

确认流程注册和 Lely PDO API：

```sh
grep -R "heartbeatProcess\|sdoProcess\|pdoProcess" src include
grep -R "OnRpdo\|OnTpdo\|RpdoMapped\|TpdoMapped\|WriteEvent" \
    src/pdo_process.cpp include/pdo_process.h
```

确认 A03 没有绕过 Lely 构造 SocketCAN PDO 帧：

```sh
grep -R "cansend\|struct can_frame\|PF_CAN\|SOCK_RAW" \
    src/pdo_process.cpp include/pdo_process.h
```

预期无结果。

确认流程私有参数不再进入公共配置：

```sh
grep -R "CANOPEN_HEARTBEAT_PERIOD_MS\|CANOPEN_HEARTBEAT_MULTIPLIER" \
    include src

grep -n "kHeartbeatIndex\|kHeartbeatTimeoutMs\|kHeartbeatSampleCount" \
    src/nmt_heartbeat.cpp
grep -n "kTestObjectIndex\|kProbeValue" src/sdo_process.cpp
```

第一条预期无结果；后两条必须命中对应流程文件。

Host 语法检查：

```sh
for f in src/*.cpp; do
    c++ -std=c++14 -Wall -Wextra -Wpedantic -Wconversion \
        -Wsign-conversion -Wshadow -Werror=return-type -fsyntax-only \
        -Iinclude -isystem <lely-include> \
        -isystem third_party/spdlog/include "$f" || exit 1
done
```

Host `-fsyntax-only` 只验证语法、模板实例化和工程内部接口，不代表 Yocto/aarch64 完整链接或目标板运行通过。

正式环境仍需执行：

```sh
rm -rf build
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --verbose -j"$(nproc)"
```

## 目标板公共前置

同时启动主站日志和 CAN 抓包：

```sh
candump -t A -x can1
cmake --build build --target download
```

Startup Boot 必须成功，且 CAN controller 不得进入 bus-off。

## A01 Heartbeat

通过条件：

1. 总线正常存在主站 `0x77F` 与从机 `0x701` Heartbeat；
2. A01 稳定等待覆盖 5 个 500 ms Heartbeat 周期；
3. 停止主站 Producer Heartbeat 后，节点 1 在 3000 ms 内上报 `0x8130` EMCY；
4. 恢复主站 Producer Heartbeat 后，节点 1 在 3000 ms 内上报 `0x0000` EMCY reset；
5. SDO 写节点 1 `0x1017:00=0` 后，主站在 3000 ms 内收到 `OnHeartbeat(node=1, true)`；
6. SDO 写回 `0x1017:00=500` 后，主站在 3000 ms 内收到 `OnHeartbeat(node=1, false)`；
7. 本地主站 `0x1F81:01` 最终恢复为 `0x00000005`。

## A02 SDO

通过条件：

1. 能读取并保存节点 1 `0x2200:00`；
2. 写入与原值不同的 probe value；
3. SDO read-back 与 probe 一致；
4. 测试结束恢复原值；
5. 恢复后 SDO read-back 与保存的原值一致；
6. 任一 local completion wait timeout 不得被报告为恢复成功。

## A03 TPDO1

当前被测映射：

```text
COB-ID 0x181
DLC 8
bytes 0..3 = 0x2100:00 UNSIGNED32 little-endian
bytes 4..7 = 0x2101:00 UNSIGNED32 little-endian
event timer = 1000 ms
```

通过条件：

1. 在约 7000 ms 采样窗口内至少接收 5 个有效 TPDO1；
2. 每个 callback 的 PDO number 为 1 且无 Lely processing error；
3. DLC 必须等于 8；
4. 4 个连续周期全部位于 850~1150 ms；
5. raw payload 解码值与 `RpdoMapped(node 1)` 值一致；
6. 稳定 TPDO generation 窗口内，raw payload 与 SDO `0x2100:00`、`0x2101:00` 一致；
7. 比较期间若收到新 TPDO，流程必须重试而不是直接使用跨周期数据。

预期抓包持续出现：

```text
181#.................
```

实际 8-byte payload 以目标 OD 当前值为准。

## A03 RPDO1

当前被测映射：

```text
COB-ID 0x201
DLC 4
bytes 0..3 = 0x2200:00 UNSIGNED32 little-endian
```

通过条件：

1. A03 发送前先通过 SDO 保存 `0x2200:00`；
2. `TpdoMapped()` 写入与原值不同的 probe；
3. `WriteEvent()` 后收到 `OnTpdo(PDO1)` completion；
4. callback payload DLC=4 且 little-endian 解码等于 probe；
5. 目标板抓包出现 `0x201`；
6. SDO read-back `0x2200:00 == probe`；
7. 最终通过 SDO 恢复原值并回读确认；
8. 如果 SDO local completion wait 状态未知，只允许 best-effort PDO 恢复，A03 仍判定失败，不得宣称清理 PASS。

## A03 NMT 与清理

A03 开始时本地主站 Node-ID 127 切到 Operational，并向节点 1发送 NMT Start。结束时必须：

- 注销 A03 的 `OnRpdo()`/`OnTpdo()` callback；
- 本地主站保持 Operational，不得因阶段清理触发广播 Reset Communication；
- 从机 `0x2200:00` 恢复并验证原值，或明确报告恢复未验证；
- 不修改 EDS、DCF 或持久化参数。

## Final Reset

按 `Ctrl+C` 后预期：

```text
Final Reset Communication sent to node 1
Boot callback: node=1 ...
Final Reset Communication completed
```

随后 event loop 退出，工作线程完成 `join()`。若未收到退出信号而 event loop 提前停止，进程必须记录错误并返回失败。

## 验收矩阵

| 编号 | 项目 | 通过判据 |
| ---: | --- | --- |
| 1 | Startup Boot | 状态 0 或 `L` |
| 2 | A01 主站 Heartbeat timeout | 节点 1 上报 `0x8130` |
| 3 | A01 主站 Heartbeat recovery | 节点 1 上报 `0x0000` |
| 4 | A01 从机 timeout/recovery | `OnHeartbeat(..., true/false)` 均在 3000 ms 内发生 |
| 5 | A02 write/read-back | `0x2200` probe 一致 |
| 6 | A02 restore | 原值恢复并回读一致 |
| 7 | A03 TPDO receive | 5 帧、DLC 8、无 processing error |
| 8 | A03 TPDO timing | 4 个间隔均为 1000+/-150 ms |
| 9 | A03 TPDO mapping | raw、Lely mapped、SDO OD 一致 |
| 10 | A03 RPDO transmit | `0x201` DLC 4，payload 等于 probe |
| 11 | A03 RPDO apply | SDO `0x2200` 回读等于 probe |
| 12 | A03 cleanup | `0x2200` 原值恢复并验证，本地主站保持 Operational，且不触发广播 Reset Communication |
| 13 | 正常退出 | Final Reset Communication 完成 |
| 14 | 完整验证 | 交叉构建、目标抓包和本地多轮复审均有实际证据 |
