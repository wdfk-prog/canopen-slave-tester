# CANopen Host 自动测试设计

## 设计边界

主 CANopen 流程复用单个 `CanopenTestMaster`/`AsyncMaster`，不创建第二套 CANopen protocol Service。`main.cpp` 负责 Lely/SocketCAN 生命周期、Startup Boot、流程注册和退出；各验证流程持有测试私有参数和断言逻辑。GFC protocol validation 是唯一需要 wire-level fixture 的当前验证流程：它在同一 `can1` 上创建第二个独立 `CanChannel`，只处理固定 CAN-ID `0x001`，不分配第二个 CANopen Node-ID。

Host 支持两个编译角色，共享同一 CMake 和 `src/*.cpp`。角色只由 `include/canopen_config.h` 的 `CANOPEN_ROLE` 选择：Master 角色执行当前启用的 validation process；Slave 角色使用 Lely `BasicSlave` Node 2 验证 MCU NMT Master。Master 在 `runCanopenMaster()` 中绑定统一 callable process table；GFC protocol validation 额外绑定 process-local wire channel，NMT Master 测试不进入该表。详细设计见 [CANopenNode NMT Master 测试设计](CANopen_NMT_Master_Test.md)。

Master 验证流程的固定注册顺序如下；实际执行项由对应 enable 宏决定：

```text
Startup Boot
→ Heartbeat validation
→ SDO object-access validation
→ PDO validation
→ SYNC/synchronous PDO validation
→ TIME consumer validation
→ EMCY producer validation
→ EMCY consumer validation
→ GFC protocol validation
→ 等待 Ctrl+C/SIGTERM
→ Final Reset Communication
```

注册流程采用 fail-fast：当前流程返回非零时，不执行后续自动流程。

## 文件职责

```text
src/main.cpp
├─ 创建并持有全部 Lely 和 SocketCAN 对象
├─ 注册 CAN/Boot/Heartbeat callback 和共享 EMCY observer
├─ 启动 Loop::run() 工作线程
├─ Reset 并等待 Startup Boot
├─ canopenRunProcesses(enabled validation processes, including GFC protocol validation)
├─ 等待退出信号
├─ finalResetProcess()
└─ shutdown Context 并 join 线程

src/canopen_emcy.cpp
├─ 独占 Lely OnEmcy() callback
├─ 固定容量 EMCY event cache、sequence 与 monotonic timestamp
└─ Heartbeat/EMCY validation 共用 sequence-based wait

src/nmt_heartbeat.cpp
├─ Boot/Heartbeat callback 和等待状态
├─ Heartbeat validation 私有 Heartbeat 参数
├─ 主站 Producer Heartbeat 中断/恢复
└─ 通过 SDO 中断/恢复从机 Producer Heartbeat

src/sdo_process.cpp
├─ SDO object-access validation 私有测试对象和 probe value
├─ SubmitRead/SubmitWrite completion 等待
└─ 0x2200:00 写入、回读、恢复和恢复确认

src/pdo_process.cpp
├─ PDO validation 私有 PDO/OD/时序参数
├─ OnRpdo/OnTpdo callback 状态
├─ TPDO1 payload/周期/映射/OD 一致性检查
├─ Lely TpdoMapped() RPDO1 发送
└─ 0x2200:00 保存、回读和恢复

src/sync_pdo_process.cpp
├─ SYNC/synchronous PDO validation 私有 SYNC/PDO/时序参数
├─ 0x1005/0x1019 producer-consumer 拓扑校验
├─ TPDO1 0x1800 参数保存、同步切换与恢复
├─ OnSync/OnRpdo 同步时序观测
└─ 恢复后事件型 TPDO1 周期验证

src/emcy_process.cpp
├─ EMCY producer validation 读取 0x1001/0x1003/0x1014/0x1015 基线
├─ Heartbeat timeout 仅作为确定性 EMCY fault source
├─ 从机 0x1014 与主站本地 0x1028:01 联动切换
├─ 0x1015 inhibit timestamp 断言
└─ 全量恢复、回读和原 COB-ID smoke test

src/shutdown_process.cpp
└─ Reset Communication 并等待 Boot callback
```

## 流程私有配置

公共 `include/canopen_config.h` 只保留跨流程共享的运行环境配置和流程开关。阶段内部测试参数放在对应 `.cpp` 的匿名命名空间。

Heartbeat validation 当前私有参数包括：

```text
0x1017 Producer Heartbeat object
Heartbeat wait timeout = 3000 ms
Heartbeat activation sample count = 5
Producer Heartbeat period = 500 ms
```

SDO object-access validation 当前私有参数包括：

```text
0x2200:00 test object
primary probe = 0x12345678
alternate probe = 0x87654321
```

PDO validation 当前私有参数包括：

```text
TPDO1 remote objects = 0x2100:00 + 0x2101:00
RPDO1 remote object = 0x2200:00
TPDO samples = 5
TPDO expected period = 1000 ms
TPDO tolerance = +/-150 ms
RPDO probe = 0xA5A5A5A5 / alternate 0x5A5A5A5A
```

SYNC/synchronous PDO validation 当前私有参数包括：

```text
SYNC period = 200 ms
SYNC samples = 5
TPDO1 temporary transmission type = 1
quiet window >= 1500 ms
restored event timer tolerance = +/-150 ms
```

`master.yml`、EDS 和 DCF 继续负责真正的 CANopen 通信参数；源码私有常量只描述测试策略，不替代 DCF 配置。

## Startup Boot

启动顺序保持：

```text
注册 callback
→ 启动 event loop
→ prepareBootWait()
→ master.Reset()
→ waitForBootCompletion()
```

Boot callback `status == 0` 或 `status == 'L'` 视为可继续自动流程。其他结果或等待超时阻止 core CANopen protocol validation suite/EMCY consumer validation。

## Heartbeat validation

Heartbeat validation 首先等待 5 个 500 ms Producer Heartbeat 周期，使双向 Heartbeat consumer 进入稳定监督状态，然后验证：

```text
prepare 0x8130 wait
→ local master 0x1017:00 = 0
→ wait slave EMCY 0x8130
→ prepare 0x0000 wait
→ local master 0x1017:00 = 500
→ wait slave EMCY reset
→ NMT Start node 1
```

随后验证从机 Producer Heartbeat：

```text
local 0x1F81:01 = 0
→ SubmitWrite(node 1, 0x1017:00, 0)
→ wait OnHeartbeat(node=1, true)
→ SubmitWrite(node 1, 0x1017:00, 500)
→ wait OnHeartbeat(node=1, false)
→ local 0x1F81:01 = 0x00000005
```

测试窗口临时清除本地主站的 slave assignment，避免 synthetic Heartbeat timeout 触发 Lely 默认 NMT reset。测试结束后恢复 assignment。

## SDO object-access validation

SDO object-access validation 使用 `SubmitRead<uint32_t>()` / `SubmitWrite()` 验证节点 1 的 `0x2200:00`：

```text
read original
→ choose probe != original
→ write probe
→ read-back == probe
→ restore original
→ read-back == original
```

协议级 SDO timeout 与本地 completion wait timeout 分开处理。本地 completion wait timeout 时远端事务状态被标记为未知，不继续提交可能冲突的恢复事务。

## PDO validation RPDO/TPDO

### PDO 映射基线

当前 EDS/DCF 已提供：

```text
Slave TPDO1 0x181, DLC 8
  0x2100:00 UNSIGNED32
  0x2101:00 UNSIGNED32
  event timer 1000 ms

Slave RPDO1 0x201, DLC 4
  0x2200:00 UNSIGNED32
```

主站 DCF 具有对应远端映射，因此 PDO validation 不修改 EDS/DCF。

### NMT 状态

生成的主站 DCF 配置为 `start:false`，主站自身默认保持 Pre-operational。Lely PDO 服务要求本地节点处于 Operational，因此 PDO validation 执行：

```text
NMT Start local master Node-ID 127
→ NMT Start slave Node-ID 1
→ PDO verification
→ keep local master Operational for later automatic stages
```

当 `BasicMaster::Command()` 的目标 Node-ID 等于主站自身 Node-ID 时，Lely 在本地执行 NMT 状态切换，不发送针对 Node 127 的网络 NMT 命令。

### TPDO1 验证

PDO validation 在进入 Operational 前注册 `OnRpdo()`。从主站视角，从机 TPDO1 是主站 RPDO1。

`OnRpdo()` callback 只复制 PDO number、错误、payload 和 `steady_clock` 时间戳并唤醒等待线程，不执行 SDO 或长时间断言。

收集 5 个 TPDO1 后检查：

- callback 无处理错误；
- DLC 等于 8；
- payload `[0..3]` 按 little-endian 解码为 `0x2100:00`；
- payload `[4..7]` 按 little-endian 解码为 `0x2101:00`；
- 4 个连续周期均在 850~1150 ms。

随后比较：

```text
raw TPDO payload
== master.RpdoMapped(node 1) mapped value
== SDO read 0x2100:00 / 0x2101:00
```

若比较期间收到新 TPDO，则放弃当前快照并使用最新 generation 重试，避免跨周期比较旧 payload 和新 OD 值。

### RPDO1 验证

PDO validation 先通过 SDO 保存 `0x2200:00`，然后选择一个与原值不同的 probe：

```text
TpdoMapped(node 1)[0x2200][0].Write(probe)
→ WriteEvent()
→ wait OnTpdo(PDO1)
→ validate DLC=4 and little-endian payload
→ SDO read 0x2200:00 == probe
```

主站 `TpdoMapped()`/`OnTpdo()` 对应从机 RPDO1，因为 PDO 命名始终以本地主站方向为准。

正常清理使用 SDO 恢复原 `0x2200:00` 并回读确认。若本地 SDO completion wait 状态未知，则不继续叠加 SDO 事务，改用 RPDO 做一次 best-effort 原值恢复，但该路径无法完成 SDO read-back，因此 PDO validation 仍返回失败。

PDO validation 结束时只注销 `OnRpdo()`/`OnTpdo()` callback；本地主站保持 Operational，供后续 PDO/SYNC 等自动阶段继续使用。实机验证表明将本地主站切回 Pre-operational 会触发 Lely master boot 管理并广播 Reset Communication，因此阶段清理不得执行该状态回退。

## SYNC/synchronous PDO validation 与同步 TPDO

SYNC/synchronous PDO validation 复用 PDO validation 后保持 Operational 的本地主站，但也能在 PDO validation 被编译关闭时自行向 Node-ID 127 发送本地 NMT Start。测试前先读取：

```text
slave  0x1005 / 0x1019
master 0x1005 / 0x1006 / 0x1019
```

必须满足：

- 主站 `0x1005` producer bit 为 1；
- 从机 `0x1005` producer bit 为 0，即保持 SYNC consumer；
- 主从 SYNC COB-ID/帧格式一致；
- 主从 `0x1019` 一致且当前为 0，不使用 SYNC counter byte。

该检查用于区分“固件具备 `PKG_CANOPENNODE_SYNC_PRODUCER` 能力”和“当前节点实际作为 producer 运行”。SYNC/synchronous PDO validation 不允许主从同时成为 SYNC producer。

随后进入：

```text
slave -> Pre-operational + fresh OnState(PREOP) confirmation
→ save 0x1800:00/01/02/03/05/06
→ disable TPDO1 by 0x1800:01 bit31
→ 0x1800:02 = 1
→ restore valid 0x1800:01
→ read-back COB-ID/type
→ register OnSync/OnRpdo/OnSyncError
→ slave -> Operational
```

本地主站先写本地 `0x1006=0`，在至少 `max(1500 ms, original_event_timer + 300 ms)` 的无 SYNC 窗口内要求 SYNC 和 TPDO1 都为 0。这样可以证明原来的 event timer 不会在同步 transmission type 下继续触发 TPDO。

之后本地主站通过 Lely 本地 OD 写 `0x1006=200000 us` 启动 SYNC producer。`OnSync()` 记录每个 SYNC 时间戳；第五个 SYNC callback 将本地 `0x1006` 写回 0，停止周期发送。`OnRpdo(PDO1)` 记录从机同步 TPDO1。通过条件：

```text
SYNC count = 5
TPDO1 count = 5
SYNC[i] <= TPDO1[i] < SYNC[i+1]  (i=0..3)
SYNC[4] <= TPDO1[4] < SYNC[4] + 200 ms
```

再等待一个 SYNC period，数量必须保持 5/5，排除额外发送。

清理时先停止本地主站 SYNC，再向从机发送 Enter Pre-operational，并等待新的 `OnState(PREOP)` 远端状态事件；确认后才按 disable/restore-type/enable 顺序恢复 TPDO1。随后回读 `0x1800:00/01/02/03/05/06`，必须与原快照逐项一致，并确认从机 `0x1005` 未变化。最后从机重新进入 Operational，在无 SYNC 条件下接收两个 TPDO1，并验证二者间隔等于原 event timer `+/-150 ms`。本地主站 `0x1006` 最终恢复为进入 SYNC/synchronous PDO validation 前保存的值。

从第一笔 TPDO1 修改型 SDO 发起前开始，SYNC/synchronous PDO validation 即把从机视为“可能已修改”，因为 local completion wait timeout 不能证明远端没有执行写入。正常清理只有在进入 Pre-operational、disable/restore-type/enable 写入以及 `0x1800:00/01/02/03/05/06` 与 `0x1005` 全量回读全部成功后，才设置 restoration verified。若发生 WAIT_TIMEOUT、普通 SDO abort/timeout、NMT Pre-operational 失败或恢复回读不一致，只要 restoration 尚未验证，SYNC/synchronous PDO validation 都会停止本地主站 SYNC，并以一次有界的从机 Reset Communication 作为更强恢复边界。Lely 对 mandatory slave 的 Boot 管理可能在 Reset Communication 后自动把节点带回 Operational（Boot callback 可报告 `status='L'`），因此 recovery 在 Boot 完成后会再次明确发送 Enter Pre-operational，并等待新的 `OnState(PREOP)` 状态事件后再重新全量回读；仅 `master.Command()` 接受命令不能视为状态已经切换。只有 restoration verified 后 Step 16 才允许最终 NMT Start。即使 Reset 恢复成功，本次 SYNC/synchronous PDO validation 仍保持 FAIL；若 Reset/Boot/Pre-operational/回读再次失败，则明确报告恢复未验证并拒绝显式 NMT Start。

NMT 状态确认存在一个已知且当前接受的极低概率并发边界。`issueNmtCommandAndWaitForState()` 的 generation 只能排除在命令基线之前**已经发布到缓存**的旧状态；Lely `OnState()` 本身没有 command token，因此如果一个旧的 `PREOP` callback 在本次 `ENTER_PREOP` 之前已经进入分发，但恰好阻塞在 `g_nmt_state_mutex`，它仍可能在 `Command()` 之后才发布并递增 generation，从而被当作新的 PREOP 证据。该窗口要求非常特定的线程调度顺序，当前没有实机证据或稳定复现，项目选择不为此引入额外 command epoch、第二套回调队列或更复杂的同步状态机。后续审查不把该理论竞态作为必须修复项；只有出现实际日志证据、目标板复现或相关功能异常时才重新评估。

## EMCY producer validation

EMCY producer validation 不再修改 `0x1016`，Heartbeat Consumer 的功能性覆盖留在 Heartbeat validation。EMCY producer validation 复用停止主站 Producer Heartbeat 产生 CANopenNode `0x8130`，但断言对象转为 EMCY producer 本身。

首先在原 COB-ID 上验证：

```text
0x1001 == 0 前置
→ 临时写 0x1015=0 并回读，隔离基础 EMCY 行为
→ 停止主站 0x1017
→ shared OnEmcy 收到 0x8130
→ EMCY Error Register == SDO 0x1001，communication bit 置位
→ 0x1003 newest 与 0x8130/error register/error bit 0x1B 一致
→ 保持 fault 1 s，不允许第二条 EMCY
→ 恢复主站 0x1017
→ 收到 0x0000，0x1001 清零，0x1003 newest/previous 对应 reset/fault
```

这里的 reset 历史行为是当前 CANopenNode 实现特性：`CO_error()` 在 error set/reset 两种状态切换时都会把消息压入 EMCY FIFO，`0x1003` 直接读取该 FIFO，因此一个 fault/recovery pair 会增加两条记录。EMCY producer validation 按当前被测 CANopenNode 版本验证这一行为，不把它泛化为其他 CANopen 栈的通用实现要求。

随后验证 configurable COB-ID。CANopenNode 与 Lely 都禁止“有效旧 CAN-ID直接切为另一个有效 CAN-ID”，所以顺序固定为：

```text
slave 0x1014: old -> old|bit31
master 0x1028:01: old -> old|bit31 -> 0x681
slave 0x1014: disabled old -> 0x681
```

主机和从机回读都为 `0x681` 后，写从机 `0x1015=15000`（100 us 单位，即 1.5 s），再次制造 fault 并立即恢复 Heartbeat。共享 observer 的 `steady_clock` 时间戳要求 reset EMCY 相对 fault EMCY 的间隔位于 1400..2500 ms。

清理始终恢复保存值而不是硬编码默认值：先恢复主站 Heartbeat 并轮询 `0x1001` 清零，再恢复 `0x1015`，禁用当前 slave EMCY producer，恢复 master `0x1028:01`，最后恢复 slave `0x1014`。全部回读一致后再在原 COB-ID 上执行一次 fault/reset 和 `0x1000` SDO smoke read。`0x1003` 不清空，以免删除测试前历史。

## EMCY consumer validation

EMCY consumer validation 与 EMCY producer validation 方向相反：EMCY producer validation 验证 MCU producer -> Host consumer；EMCY consumer validation 使用 Host Node 127 的 Lely EMCY producer 验证 MCU consumer -> application diagnostic。Host 不增加第二 CANopen 节点，也不构造 Raw CAN。

MCU 测试固件提供只读 diagnostic `0x2301:01..07`：

```text
01 remote_rx_count      U32
02 last_source_node_id  U8
03 last_cob_id          U16
04 last_error_code      U16
05 last_error_register  U8
06 last_error_bit       U8
07 last_info_code       U32
```

因为这些字段由多次独立 SDO upload 读取，Host 使用一致性读法：先读 `remote_rx_count`，再读 `02..07`，最后再次读取 `remote_rx_count`；只有前后计数一致才接受该 snapshot，否则最多重试 3 次。每个 Host 发送动作都要求计数精确 `+1`，因此重复内容的 EMCY 也不能被静默去重。

Host error vector 使用 manufacturer/device-specific `0xFF01/0xFF02`，避免触发 `0x81xx` communication error behavior。MSEF 使用非对称字节，分别验证 CANopenNode callback 的 `errorBit` 与 `infoCode` 字节布局。

非零 EMCY 通过 `CanopenTestMaster::pushLocalEmcy()` 产生。Lely 高层 API 对 `Error(0)` 不发送 recovery，因此工程通过 `CanopenTestMaster` 为 EMCY consumer validation 对 Lely 自己的 local EMCY service 暴露最小 `push/peek/clear`；调用保持 Lely master 锁，并由 `clear()` 发送标准 `0x0000` error-reset/no-error EMCY，不引入 Raw CAN 协议路径。

`pushLocalEmcy()` 在 master 锁内调用现有 COEmcy::push() 并返回结果，只有确认 local EMCY 已入栈后才更新 host_error_active/expected_host_error_count。

EMCY consumer validation 顺序覆盖：

```text
single vector A
-> consecutive vector B
-> recovery 0x0000
-> duplicate A + A
-> duplicate cleanup recovery
-> ordinary SDO 0x1000 smoke read
-> nonzero Vector A persistence marker
-> Reset Communication + fresh Boot with reported NMT state
-> require post-reset 0x2301 snapshot equals the full nonzero pre-reset snapshot
-> vector B + recovery
-> if Boot did not already report Operational: NMT Start + fresh OnState(START)
```

MCU `remote_rx_count` 与最后一条 EMCY snapshot 设计为跨 communication reset 保留。Host 在 fresh Boot 后先要求 `0x2301` 与 reset 前 snapshot 完全一致，再要求后续 fresh EMCY 精确 `+1`，从外部同时证明 diagnostic 持久性以及 `CO_EM_initCallbackRx()` 已绑定到新 `CO_t`。fresh Boot callback 同时保存其报告的 NMT state；如果已经是 `START`，该 Boot 结果直接证明节点已被 Lely Boot manager 恢复到 Operational，EMCY consumer validation 不再发送冗余 NMT Start，也不等待不会产生的 fresh `OnState(START)`；仅在 Boot state 不是 `START` 时才显式 Start 并等待状态事件。EMCY consumer validation preflight 只要求 Host active EMCY stack 为空，不把尚未由 Lely EMCY service 同步的 compact `0x1003:00` 初始值当作历史深度。任何失败路径都清理 EMCY consumer validation 自己产生的 Host EMCY；清理前重新检查 `0x1003:00` 深度、栈顶仍属于 EMCY consumer validation 且 combined Error Register 仍为 `0x81`，若出现其他 Host fault 则拒绝调用全栈 `clear()`。若已执行 remote Reset Communication 且 Boot 尚未证明节点已经 Operational，还必须尝试恢复节点 1 Operational。

详细用例和 HIL 判据见 [EMCY consumer validation 测试](CANopen_EMCY_Consumer_Test.md)。

## GFC protocol validation

GFC protocol validation 保持 CANopen 协议控制和 wire-level 观测分离。`CanopenTestMaster` 继续独占原 master channel，并通过 SDO 访问 MCU `0x1300/0x2302`；`main.cpp` 只在 Master role 且 GFC protocol validation 启用时，在同一 `CanController` 上打开第二个 `CanChannel(txwait=false)`。`gfc_process.cpp` 的私有 fixture 只发送/捕获 CAN-ID `0x001`，不向其他验证流程暴露通用 Raw CAN API。

Consumer 路径用固定 `0x001/DLC0` 注入并通过 `0x2302.rx_count` 精确计数；valid=0 和 DLC1 使用有界负向窗口证明不产生合法 callback。Producer 路径用 sequence-based `producer_request_seq/producer_complete_seq` 把 `CO_GFCsend()` 保留在 MCU mainline，再以第二 channel 的 timestamped wire capture 证明标准 `0x001/DLC0` 真正出现在总线上。

Reset Communication 后要求 `rx_count` 保留、producer request 不处于 pending，再发送 fresh GFC 证明新 `CO_GFC_t` callback 重绑。cleanup best-effort 恢复 Operational 和测试前 `0x1300`。该流程只验证 GFC protocol function；真实安全执行器、SIL/PL、WCET 和认证不在自动 PASS 范围。

详细用例见 [GFC protocol validation 测试](CANopen_GFC_Test.md)。

## 退出

收到 `SIGINT` 或 `SIGTERM` 后：

```text
finalResetProcess(master)
→ Reset Communication node 1
→ wait Boot callback
→ Context::shutdown()
→ join event-loop thread
```

由于 `boot:true`，Final Reset 会再次执行节点配置。主站随后退出导致节点 1 检测到主站 Heartbeat 丢失，属于真实离线行为，不计入已完成自动验证流程的结果。

## 兼容性和影响范围

- 保持单个 `AsyncMaster` 和现有 event-loop 线程模型；
- 不引入 `BasicDriver`、第二套 CANopen 协议层或外部 `cansend`；
- 不修改 CANopenNode、RT-Thread CAN core、Lely、EDS 或 DCF；从机仅需在 BSP 配置中开启 `PKG_CANOPENNODE_EM_PROD_CONFIGURABLE`；
- 不改变 CAN 接口、Node-ID、目标部署目录或 CMake target；
- Heartbeat validation 的等待参数改为流程私有常量，其中 callback/SDO wait 为 3000 ms，稳定等待为 5 个 500 ms 周期；
- SDO object-access validation 测试对象和 probe value 改为流程私有命名；
- 新增 PDO validation 流程开关和 `pdoProcess()` 工程内部接口；
- 新增 SYNC/synchronous PDO validation 流程开关和 `syncPdoProcess()` 工程内部接口；SYNC/synchronous PDO validation 只临时修改运行期 `0x1800` 和主站本地 `0x1006`，不修改 EDS/DCF；
- 新增共享 `canopen_emcy` observer 和 EMCY producer validation `emcyProcess()`；EMCY producer validation 只临时修改 `0x1014/0x1015/0x1028:01/0x1017` 并严格恢复。
- EMCY consumer validation 新增 `emcyConsumerProcess()` 与 `CanopenTestMaster` 的最小 local EMCY test shim；不修改 MCU 通信参数，只读取 `0x2301` diagnostic；
- GFC protocol validation 新增 `gfcProcess()` 与独立 fixed-ID Lely wire channel；只访问 MCU `0x1300/0x2302` 和 CAN-ID `0x001`，不修改 Lely 源码。
