# J03 / B09G GFC 协议测试

## 1. 范围

B09G 验证 CANopenNode Global Fail-safe Command (GFC) 的协议功能。测试覆盖 GFC consumer、producer、`0x1300` valid 参数、非法 DLC、连续接收、Reset Communication 后 callback 重绑和标准 wire format。

本阶段只做协议功能验证，不声明 SIL、PL、功能安全完整性、真实执行器安全停机、WCET/安全时间预算或 EN 50325-5 系统级认证。

## 2. Host / MCU 分工

Host 继续使用现有 Lely `EmcyTestMaster`/`AsyncMaster` 执行 SDO、NMT 和 Boot 控制。由于 Lely CANopen 高层 API 没有 GFC service，B09G 在同一个 `can1` 上额外创建一个独立 `lely::io::CanChannel`，只用于 CAN-ID `0x001` 的固定注入和捕获。

两个 channel 的职责固定为：

```text
master channel
    -> AsyncMaster exclusive use
    -> SDO/NMT/Boot

gfc wire channel
    -> fixed CAN-ID 0x001 send/capture
    -> DLC 0 valid frame
    -> DLC 1 negative frame
```

第二个 channel 不是第二个 CANopen 节点，不分配 Node-ID，也不提供任意 CAN-ID/payload 的通用 Raw CAN CLI。

MCU 固件必须提供：

```text
PKG_CANOPENNODE_USING_GFC=y
PKG_CANOPENNODE_GFC_CONSUMER=y
PKG_CANOPENNODE_GFC_PRODUCER=y
PKG_CANOPENNODE_DEMO_GFC_DIAGNOSTIC=y
```

以及标准对象 `0x1300:00` 和 test-only `0x2302` record。

## 3. MCU OD 契约

标准对象：

```text
0x1300:00  UNSIGNED8  RW  Global fail-safe command parameter
```

测试接受的有效值是 `0` 和 `1`；写入 `2` 必须以 SDO abort `PARAM_VAL (0x06090030)` 拒绝，并保持原值不变。

测试 record：

| Sub | Name | Type | Access | B09G 含义 |
| ---: | --- | --- | --- | --- |
| `0x00` | highestSubIndex | U8 | RO | `5` |
| `0x01` | rx_count | U32 | RO | 合法 GFC consumer callback 次数 |
| `0x02` | safe_requested | U8 | RO | 收到合法 GFC 后置 `1` |
| `0x03` | producer_request_seq | U32 | RW | Host 请求 MCU mainline 执行一次 `CO_GFCsend()` |
| `0x04` | producer_complete_seq | U32 | RO | MCU 已完成的 request sequence |
| `0x05` | producer_result | I32 | RO | 最近一次 `CO_GFCsend()` 返回值 |

`producer_request_seq` 只触发 MCU mainline 发送；OD write callback 不直接调用 `CO_GFCsend()`。

## 4. Wire fixture

B09G fixture 只支持：

```text
send CAN-ID 0x001, DLC 0
send CAN-ID 0x001, DLC 1, data[0]=0
capture CAN-ID 0x001 with timestamp
drain queued frames
```

Producer 的最终发送证据必须来自 wire capture，而不是只根据 `producer_result` 判断。

## 5. 自动测试顺序

### B09G-00 Preflight

1. drain wire channel；
2. 读取并保存 `0x1300:00`；
3. 读取 `0x2302:01..05`；
4. 要求 `producer_request_seq == producer_complete_seq`。

### B09G-01 valid=1 consumer

```text
0x1300 = 1
Host fixture -> 0x001 / DLC0
MCU rx_count -> exactly +1
safe_requested -> 1
```

### B09G-02 valid=0 consumer

```text
0x1300 = 0
Host fixture -> 0x001 / DLC0
bounded negative window
rx_count unchanged
```

随后写 `0x1300=2`。只有收到 SDO abort `PARAM_VAL (0x06090030)` 才视为合法拒绝；提交失败、timeout、其他 abort 或 unknown transaction state 均直接 FAIL。回读必须仍为 `0`。

### B09G-03 invalid DLC

```text
0x1300 = 1
Host fixture -> 0x001 / DLC1 / data=00
bounded negative window
rx_count unchanged
```

### B09G-04 producer

先验证 `valid=0` producer gate：

```text
0x1300 = 0
request_seq++
wait complete_seq
producer_result == 0
bounded wire window contains no 0x001
```

再验证有效 producer：

```text
0x1300 = 1
request_seq++
wait complete_seq
producer_result == 0
wire capture -> standard CAN-ID 0x001, flags 0, DLC 0
```

### B09G-05 continuous GFC

连续注入 3 个有效 GFC，每次都必须独立等待 `rx_count` 精确 `+1`；不允许只检查最终计数大于 baseline。

### B09G-06 Reset Communication rebind

```text
preserve rx_count
prepare Boot wait
NMT RESET_COMM Node 1
wait fresh Boot
require rx_count unchanged
require producer_request_seq == producer_complete_seq
ensure 0x1300 = 1
inject fresh GFC
require rx_count +1 and safe_requested=1
restore Operational when Boot did not already report START
```

该用例证明 test diagnostic 状态跨 communication reset 保留，并从 Host 外部证明新 `CO_GFC_t` 上的 consumer callback 已重新绑定。

### B09G-07 ordinary protocol health / A-stage regression

`gfcProcess()` 内先读取 `0x1000:00`，要求普通 SDO upload 成功，作为 B09G 的最小协议健康 smoke。

“GFC 不影响普通 A-stage 协议运行”属于 J03 完整 HIL 回归，不在 `gfcProcess()` 内递归调用 A01～A06。B09G 自动 PASS 后必须按当前项目选定的 A-stage regression profile 再执行既有 A-stage 测试；该回归未执行时，只能声明 B09G protocol smoke PASS，不能声明 B09G-07 完整回归 PASS。

## 6. Cleanup

B09G 保存测试前 `0x1300`。无论成功或失败，都执行 best-effort cleanup：

1. 如果已发出 Reset Communication 且尚未证明 MCU Operational，尝试 NMT Start 并等待 fresh START；
2. 检查 `producer_request_seq/producer_complete_seq`；如果仍有 pending request，先临时设置 `0x1300=0`，有界等待 sequence 收敛；
3. 恢复原 `0x1300`；
4. SDO 回读确认恢复值。

如果 producer sequence 无法确认收敛，B09G 保持 FAIL，并且不重新启用 GFC producer；只有 sequence 已收敛后才恢复原 `0x1300`。其他恢复失败同样使 B09G 最终结果为 FAIL。测试不执行 `0x1010` store，不持久化 GFC test value。

## 7. 完整 HIL 边界

自动 PASS 要求：

- `0x1300` valid/invalid 行为符合契约；
- valid consumer 精确 callback；
- valid=0 与 DLC1 不产生合法 callback；
- producer valid=0 无 wire frame；
- producer valid=1 实际发出 `0x001/DLC0`；
- 连续 GFC 不漏计；
- communication reset 后 callback 重新绑定；
- `0x1000` SDO health 正常；
- cleanup 先以 `0x1300=0` 收敛 producer request/complete，再恢复原 `0x1300`。

完整目标板 HIL 还需要结合 CAN state 日志和外部抓包确认测试期间未进入 passive/bus-off。该证据不扩展为 SIL/PL/安全认证结论。
