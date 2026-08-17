# B06 EMCY Consumer 测试

## 1. 目的

B06 验证 MCU CANopenNode EMCY Consumer 从 CAN 接收到 application diagnostic 的完整交付路径。Host 使用现有 Master role Node 127 作为 EMCY producer，MCU Node 1 作为 EMCY consumer。

方向与 A06 相反：

```text
A06: MCU EMCY Producer -> Host OnEmcy consumer
B06: Host EMCY Producer -> MCU EMCY Consumer -> MCU OD 0x2301 -> Host SDO assertion
```

B06 不增加第二个物理节点，不创建新的 Host role，也不使用 Raw CAN。

## 2. MCU 固件契约

MCU 测试固件必须启用：

```text
PKG_CANOPENNODE_EM_CONSUMER
PKG_CANOPENNODE_DEMO_EMCY_CONSUMER_DIAGNOSTIC
```

并提供只读 manufacturer diagnostic record：

| Sub | Name | Type | B06 含义 |
| ---: | --- | --- | --- |
| `0x01` | `remote_rx_count` | U32 | 每次 remote EMCY callback 增加 1，包括 duplicate 和 recovery |
| `0x02` | `last_source_node_id` | U8 | 最近 EMCY producer Node-ID |
| `0x03` | `last_cob_id` | U16 | 最近 EMCY CAN-ID/COB-ID |
| `0x04` | `last_error_code` | U16 | EMCY error code |
| `0x05` | `last_error_register` | U8 | EMCY Error Register |
| `0x06` | `last_error_bit` | U8 | CANopenNode callback 的 EMCY byte 3 |
| `0x07` | `last_info_code` | U32 | CANopenNode callback 的 EMCY bytes 4..7 |

Host 不修改这些对象。

## 3. Snapshot 一致性

一个 diagnostic snapshot 需要 8 次 SDO upload，因此不能假设字段天然来自同一条 EMCY。Host 固定按以下顺序读取：

```text
count_before = 0x2301:01
fields       = 0x2301:02..07
count_after  = 0x2301:01
```

只有 `count_before == count_after` 时才接受 snapshot；否则重试，最多 3 次。B06 每次发帧还要求 `remote_rx_count` 相对当前 baseline 精确 `+1`，出现其他变化立即 FAIL。

MCU diagnostic 状态设计为跨 communication reset 保留。Host 必须确认 Reset Communication 前后的 `remote_rx_count` 与最后一条 EMCY snapshot 完全一致，然后再判断 fresh EMCY 是否增加 1。

## 4. Host producer 约束

Host 使用 Node 127，B06 开始前要求：

```text
0x1014 enabled
0x1014 CAN-ID == 0x080 + 127 == 0x0FF
0x1015 == 0
Host local EMCY stack empty
```

B06 不自动改写这些参数；不满足时直接 FAIL，避免测试覆盖真实配置错误或清除既有 Host fault/history。

非零 EMCY 由：

```cpp
bool stack_updated = false;
const int result = master.pushLocalEmcy(error_code, error_register, msef, stack_updated);
```

产生。

`pushLocalEmcy()` 在 master 锁内调用现有 COEmcy::push() 并返回结果，只有确认 local EMCY 已入栈后才更新 host_error_active/expected_host_error_count。

Lely `AsyncMaster::Error(0, ...)` 不产生 recovery。工程的 `EmcyTestMaster` 仅作为 B06 access shim，在 Lely master 锁内访问现有 local EMCY service，并调用 `clear()` 清空 B06-owned error stack；Lely service 随后发送标准 `0x0000` error-reset/no-error EMCY。这个扩展不暴露任意 CAN frame 发送能力。

## 5. Test vectors

Vector A：

```text
EEC  = 0xFF01
ER   = 0x81
MSEF = A1 12 34 56 78
```

期望：

```text
source_node_id = 127
cob_id         = 0x0FF
error_code     = 0xFF01
error_register = 0x81
error_bit      = 0xA1
info_code      = 0x78563412
```

Vector B：

```text
EEC  = 0xFF02
ER   = 0x81
MSEF = B2 87 65 43 21
```

期望：

```text
error_bit = 0xB2
info_code = 0x21436587
```

使用 `0xFFxx` 避免 `0x81xx` communication EMCY 额外触发 Host Error Behavior object。

## 6. 自动测试顺序

### B06-01 / B06-02：single EMCY 与 MSEF

1. 读取稳定 baseline；
2. Host 发送 Vector A；
3. 等待 `remote_rx_count == baseline + 1`；
4. 读取稳定 snapshot；
5. 校验 Node-ID、COB-ID、EEC、ER、errorBit、infoCode。

### B06-03：连续不同 EMCY

不清除 Vector A，直接发送 Vector B；必须再精确 `+1` 并匹配 Vector B。

### B06-05：recovery

调用 Lely local EMCY `clear()`；必须收到一条新的 MCU callback：

```text
error_code     = 0
error_register = 0
error_bit      = 0
info_code      = 0
```

source Node-ID 和 COB-ID 仍应为 Host Node 127 / `0x0FF`。

### B06-04：duplicate EMCY

发送 Vector A 两次，每次分别等待一次 `remote_rx_count +1`。相同 payload 不允许被 MCU diagnostic 去重。完成后再发送 recovery 并验证。

### B06-07：SDO server health

至少一次 EMCY callback 后读取 MCU `0x1000:00`；SDO upload 必须成功。

### B06-08：Reset Communication rebind

```text
send and verify nonzero Vector A persistence marker
-> preserve that full 0x2301 snapshot
-> prepare Boot waiter
-> directed RESET_COMM Node 1
-> wait fresh Boot and capture its reported NMT state
-> require the full nonzero 0x2301 snapshot is unchanged
-> send Vector B
-> require preserved count + 1 and matching fields
-> clear Host EMCY
-> require recovery + 1
-> if Boot did not already report START: NMT Start Node 1
-> if NMT Start was required: wait fresh OnState(START)
```

使用非零 marker 可同时验证 count、source/COB-ID 与 EEC/ER/errorBit/infoCode 都跨 communication reset 保留；随后 fresh Vector B 证明 EMCY Consumer callback 已重新绑定。

### B06-09：计数与一致性

不是独立报文测试；贯穿所有用例：

- 每个合法 EMCY 精确增加一次 count；
- duplicate 同样逐帧增加；
- snapshot 前后 count 必须一致；
- 不要求 `generation == rxCount`，因为 MCU 当前契约没有 generation 字段。

## 7. Timeout

B06 私有 timeout：

```text
SDO protocol timeout       500 ms
SDO completion margin      100 ms
single EMCY observation   2000 ms
poll interval               20 ms
snapshot retry count         3
```

任一 SDO transaction 返回 error/timeout 后立即 FAIL，不继续依赖固定 sleep 猜测 remote 状态。

## 8. Cleanup

B06 不写 MCU `0x1014/0x1015/0x1016`。

从第一次 Host `pushLocalEmcy()` 开始，只要流程中途失败，就必须尝试清除 B06 产生的 Host local EMCY。若已经执行过 Reset Communication，fresh Boot callback 报告的 NMT state 会直接用于判断节点是否已经由 Lely Boot manager 恢复到 Operational；只有尚未确认 Operational 时，cleanup 才执行 NMT Start + fresh state wait。

B06 开始前如果发现 Host local EMCY active stack 非空，则直接拒绝测试，不得用 cleanup 删除测试开始前存在的真实 Host fault。生成的 compact `0x1003` 在 Lely EMCY service 第一次同步前可能仍反映 DCF 的初始 compact 值，因此 preflight 不把初始 `0x1003:00` 当作 active stack 深度。

每次调用 `clear()` 前还会重新检查当前 `0x1003:00` 深度、栈顶 error code 和 combined Error Register。只有 active stack 深度与 B06 已发送但尚未清理的 error 数一致、栈顶仍是 B06 的 `0xFF01/0xFF02` 且 register 仍为 `0x81` 才允许清理；如果测试期间出现其他 Host fault，B06 FAIL 并拒绝清空整个 local EMCY stack。

Lely `co_emcy_clear()` 对非空 stack 的顺序是先清 local EMCY stack/`0x1003`，再尝试发送 recovery。因此 recovery transmission 返回失败时，Host 本地 stack 已经为空；B06 保持失败结果并明确记录该发送错误，不会把空 stack 当成仍可通过再次 `clear()` 重发 recovery。

## 9. 不覆盖的能力

第一版不测试：

```text
short DLC / malformed EMCY
arbitrary non-CiA-301 EMCY COB-ID
multi-node EMCY Consumer
application alarm dedup policy
functional safety behavior
```

这些能力需要后续 wire-level helper 或明确产品需求后再扩展。

## 10. 自动 PASS 与完整 HIL 条件

`emcyConsumerProcess()` 自动 PASS 必须同时满足：

1. Vector A/B 字段完整一致；
2. duplicate A+A 分别产生 callback；
3. `0x0000` recovery 可观察；
4. `0x1000` SDO health 正常；
5. RESET_COMM 后 `0x2301` count/last snapshot 保持不变，并再次收到 fresh EMCY；
6. post-reset recovery 正常；
7. 最终 MCU 回到 Operational；
8. Host local EMCY stack 无 B06 残留；
9. 无 Raw CAN/cansend 路径。

完整目标板 HIL 还必须结合现有全局 CAN state 日志和 CAN 抓包确认测试期间未进入 passive/bus-off。该项是 B06 自动返回码之外的总线健康证据，当前 `emcyConsumerProcess()` 不把 CAN state transition 锁存为内部断言。
