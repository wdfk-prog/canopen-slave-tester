# A01-A06 自动测试验收

## 本地静态验收

确认流程注册和 Lely PDO API：

```sh
grep -R "heartbeatProcess\|sdoProcess\|pdoProcess\|syncPdoProcess\|timeProcess\|emcyProcess" src include
grep -R "OnRpdo\|OnTpdo\|RpdoMapped\|TpdoMapped\|WriteEvent" \
    src/pdo_process.cpp include/pdo_process.h
```

确认 A03/A04 没有绕过 Lely 构造 SocketCAN PDO/SYNC 帧：

```sh
grep -R "cansend\|struct can_frame\|PF_CAN\|SOCK_RAW" \
    src/pdo_process.cpp include/pdo_process.h \
    src/sync_pdo_process.cpp include/sync_pdo_process.h
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

## A04 SYNC 与同步 TPDO1

当前基线要求：

```text
master 0x1005 = 0x40000080  SYNC producer
master 0x1006 = 0           idle before test
slave  0x1005 = 0x00000080  SYNC consumer
master/slave 0x1019 = 0      no SYNC counter byte
slave TPDO1 0x1800:02 = 254  event-driven baseline
slave TPDO1 0x1800:05 = 1000 ms
```

通过条件：

1. 运行时读取确认主站 producer bit 为 1、从机 producer bit 为 0；
2. 主从 SYNC COB-ID/帧格式一致，`0x1019` 均为 0；
3. 保存 `0x1800:00/01/02/03/05/06` 成功；
4. Enter Pre-operational 命令后必须观察到新的远端 `OnState(PREOP)` 事件，再按 disable -> transmission type 1 -> enable 顺序配置成功并回读；若该状态确认超时，本次 A04 判 FAIL，且在尚未修改 TPDO1 的情况下仍必须执行最终 Start 清理；
5. 主站 `0x1006=0` 的 quiet window 内没有 SYNC，也没有 TPDO1；
6. Lely 主站发送 5 个 200 ms 周期 SYNC，`OnSync()` 正好观察 5 次；
7. `OnRpdo(PDO1)` 正好观察 5 个 DLC 8、无 processing error 的 TPDO1；
8. 每个 TPDO1 均发生在对应 SYNC 之后且下一 SYNC 之前；
9. 第 5 个 SYNC 后停止 producer，再等待 200 ms，SYNC/TPDO 数仍保持 5/5；
10. `0x1800:00/01/02/03/05/06` 全部恢复并回读与原值一致；
11. 从机 `0x1005` 在测试前后保持一致；
12. 无 SYNC 条件下恢复后的两个事件型 TPDO1 间隔为原 event timer `+/-150 ms`；
13. 本地主站 `0x1006` 恢复为 A04 进入前原值；
14. 任一 local SDO completion wait timeout 必须判 FAIL；从第一笔 TPDO1 修改型 SDO 开始即视为“远端可能已变更”。只要正常恢复未被全快照回读证明成功（包括 WAIT_TIMEOUT、SDO abort/timeout、NMT Pre-operational 失败或回读不一致），必须执行有界 Reset Communication + Boot；考虑到 Lely Boot 管理可能自动启动 mandatory slave，Boot 后还必须再次发送 Enter Pre-operational 并等待新的 `OnState(PREOP)` 状态事件，再做全快照回读；仅 NMT 命令提交成功不足以证明远端状态已切换。只有恢复已验证时才允许最终 NMT Start。

预期抓包同步阶段近似：

```text
080#
181#................
080#
181#................
080#
181#................
080#
181#................
080#
181#................
```

quiet window 不得出现 `0x080/0x181`；恢复事件型 TPDO 后，在没有 `0x080` 的情况下重新约每 1000 ms 出现 `0x181`。

## Final Reset

按 `Ctrl+C` 后预期：

```text
Final Reset Communication sent to node 1
Boot callback: node=1 ...
Final Reset Communication completed
```

随后 event loop 退出，工作线程完成 `join()`。若未收到退出信号而 event loop 提前停止，进程必须记录错误并返回失败。

## A06 EMCY

前置：从机实际固件必须启用 `PKG_CANOPENNODE_EM_PROD_CONFIGURABLE`、`PKG_CANOPENNODE_EM_PROD_INHIBIT` 和 `PKG_CANOPENNODE_EM_HISTORY`；初始 `0x1001` 必须为 0。A06 不修改 `0x1016`。

通过条件：

1. 原 EMCY COB-ID 上先将从机 `0x1015` 临时写 0 并回读，再停止主站 Heartbeat；收到 fresh `0x8130`，EMCY Error Register 与从机 `0x1001` 一致且 communication bit 置位；
2. `0x1003:01` 对应 `0x8130`、同一 Error Register 和 CANopenNode error bit `0x1B`；保持 fault 1000 ms 不出现额外 EMCY；
3. 恢复 Heartbeat 后收到 fresh `0x0000`，`0x1001=0`，`0x1003:01/02` 对应 reset/fault；测试不得写 `0x1003:00=0` 清历史；
4. 从机 `0x1014` 先 disable，主站本地 `0x1028:01` 按 disable/change 切到 `0x681`，随后从机 `0x1014` 启用 `0x681`；两侧 readback 必须一致；
5. `0x1015=15000` 回读一致；在 `0x681` 路径上 fault 后立即恢复 Heartbeat，reset EMCY 与 fault EMCY 的单调时间戳间隔必须在 1400..2500 ms；
6. 清理恢复保存的主站 `0x1017`、从机 `0x1015`、主站 `0x1028:01` 和从机 `0x1014`，所有值均回读一致；
7. 恢复原 COB-ID 后再次完成 `0x8130 -> 0x0000` smoke test，并成功读取从机 `0x1000`；最终 `0x1001=0`。

辅助 `candump` 可确认动态阶段出现 `681#...`，但抓包不是唯一 PASS 依据，测试驱动仍全部使用 Lely/SDO。

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
| 13 | A04 SYNC topology | 主站 producer、从机 consumer，COB-ID/0x1019 一致 |
| 14 | A04 quiet window | 无 SYNC 时无同步 TPDO1 |
| 15 | A04 sync/TPDO timing | 5 SYNC 对应 5 TPDO1，均位于对应同步周期 |
| 16 | A04 cleanup | 0x1800 全快照恢复并回读，事件型 TPDO 周期恢复 |
| 17 | A06 EMCY state/history | 0x8130/0x0000 与 0x1001/0x1003 一致，无 active-error 重复 EMCY |
| 18 | A06 configurable COB-ID | slave 0x1014 与 master 0x1028:01 都切到 0x681 并可收到 EMCY |
| 19 | A06 inhibit | 0x1015=15000 时 fault/reset 间隔 1400..2500 ms |
| 20 | A06 cleanup | 0x1014/0x1015/0x1028/0x1017 全部恢复并回读，原 COB-ID smoke test 通过 |
| 21 | 正常退出 | Final Reset Communication 完成 |
| 22 | 完整验证 | 交叉构建、目标抓包和本地多轮复审均有实际证据 |
