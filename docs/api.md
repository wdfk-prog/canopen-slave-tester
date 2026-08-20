# 代码接口

当前工程不提供命令行参数、共享库 ABI 或第三方 C++ SDK。公开头文件声明工程内部流程接口，以及供这些流程复用的远端 SDO/NMT 辅助接口。

## 公共 SDO 与 NMT 辅助接口

远端 SDO 公共适配位于 `include/canopen_sdo.h`。该头文件同时拥有 SDO 专属的 `CANOPEN_SDO_TIMEOUT_MS` 和 `CANOPEN_SDO_COMPLETION_MARGIN_MS` 默认值；这两个宏不属于跨模块全局配置：

```cpp
enum class SdoOperationResult;

template <class T>
SdoOperationResult readRemoteSdo(...);

template <class T>
SdoOperationResult writeRemoteSdo(...);
```

`readRemoteSdo()` / `writeRemoteSdo()` 内部使用 Lely `SubmitRead()` / `SubmitWrite()` 向指定 Node-ID 发起远端 SDO，并由控制/流程线程通过 `condition_variable` 有界等待 completion callback；Lely event-loop 线程仍保持运行。`SDO_TIMEOUT` 表示 Lely 已完成事务并报告协议超时，`WAIT_TIMEOUT` 表示本地未观察到 completion，远端事务状态未知。

这与 `AsyncMaster::Read()` / `Write()` 不同：当前工程中的直接 `master.Read()/Write()` 用于主站自身 local OD，例如 `0x1005/0x1006/0x1F81`，不会因为该调用本身向从机发起 SDO。

NMT 公共封装位于 `include/canopen_nmt.h`：

```cpp
bool issueNmtCommand(
    lely::canopen::AsyncMaster& master,
    lely::canopen::NmtCommand command,
    std::uint8_t node_id,
    const char* description);

void registerNmtStateCallback(lely::canopen::AsyncMaster& master);

bool issueNmtCommandAndWaitForState(...);
```

`issueNmtCommand()` 只负责调用 `master.Command()` 并把 Lely 异常转换为 `false`。返回 `true` 仅表示本地主站接受了 NMT 命令请求。`registerNmtStateCallback()` 在主站初始化时占用唯一的 `OnState()` 回调槽并记录远端 NMT 状态事件；`issueNmtCommandAndWaitForState()` 在发送命令前记录**已经发布到状态缓存**的 generation，并要求之后发布新的目标状态，因此命令前已经缓存的旧状态不能误判为本次状态切换成功。该机制提供的是事件发布顺序证据，不是严格的命令事务关联：Lely `OnState()` 不携带 command token，理论上存在“旧目标状态 callback 已进入分发、但在获取本模块互斥锁前被延迟，最终在本次 `Command()` 后才递增 generation”的极窄竞态。当前项目接受这一残余风险，不为此增加更复杂的 command epoch/回调关联机制；只有出现实机日志、稳定复现或其他运行证据时再重新评估。A04 在会立即继续修改/恢复 TPDO1 的 `ENTER_PREOP` 路径使用该确认接口。

## A01 Heartbeat 与 Boot

声明位置：`include/nmt_heartbeat.h`。

```cpp
void registerNmtHeartbeatCallbacks(
    lely::canopen::AsyncMaster& master);

void prepareBootWait();

bool waitForBootCompletion(
    std::chrono::milliseconds timeout);

bool waitForBootCompletion(
    std::chrono::milliseconds timeout,
    lely::canopen::NmtState& state);

int heartbeatProcess(
    lely::canopen::AsyncMaster& master);
```

模块只注册 `OnBoot()` 和 `OnHeartbeat()`；EMCY 由共享 observer 注册并通过 sequence-based wait 提供给 A01。无输出参数的 `waitForBootCompletion()` 保持原有调用语义；带 `NmtState&` 的重载在同一个 fresh accepted Boot result 上同时返回 callback 报告的实际 NMT state，供 B06 判断 Lely Boot manager 是否已经把节点恢复为 Operational。`CANOPEN_ENABLE_HEARTBEAT_PROCESS` 作为 A01 专属注册开关位于 `include/nmt_heartbeat.h`；对象索引、等待时间、采样数量和 Producer Heartbeat 周期位于 `src/nmt_heartbeat.cpp`，不暴露为公共宏。

## A02 SDO

声明位置：`include/sdo_process.h`。

```cpp
int sdoProcess(
    lely::canopen::AsyncMaster& master);
```

A02 保存节点 1 的 `0x2200:00`，写入临时 probe value，通过 SDO 回读验证后恢复原值。`CANOPEN_ENABLE_SDO_PROCESS` 位于 `include/sdo_process.h`；测试对象和 probe value 是 `src/sdo_process.cpp` 的私有配置。

## J06 / B02 SDO Server Block Transfer

声明位置：`include/sdo_block_process.h`、`include/canopen_sdo.h`。

```cpp
int sdoBlockProcess(CanopenTestMaster& master);

template <class T>
SdoOperationResult readRemoteBlockSdo(...);

template <class T>
SdoOperationResult writeRemoteBlockSdo(...);
```

B02 直接使用 Lely `SubmitBlockRead()` / `SubmitBlockWrite()`，不实现第二套 SDO Block 状态机。测试对象为 MCU test-only `0x2304:00 DOMAIN`，覆盖 32、900、1024、1025、2048 bytes、read-only abort、Stopped timeout recovery、client abort recovery、block→expedited、block→segmented 和 100 次最大长度 round-trip。每次 run 先保存原 payload，只有 Client-SDO callback 状态和必要的 NMT 恢复状态都可确定时，才执行 block restore + read-back。`WAIT_TIMEOUT` 或远端 readiness 无法确认时，流程停止后续 SDO cleanup。

`CanopenTestMaster::cancelRemoteSdoRequests()` 只负责受控暴露 Lely 的 protected per-node Client-SDO queue cancellation，供 B02-08 fault injection 使用。`CanopenTestMaster` 是通用测试 shim，不再以 EMCY 命名；B06 local EMCY、J04 Host Server-SDO fixture 和 B02 Client-SDO cancellation 都集中在这一层访问 Lely protected service，但不实现第二套协议。调用前必须保证同一 Node-ID 没有其他 SDO request。Lely `CancelAll()` 的返回计数不包含被停止的 ongoing request，因此 helper 只确认 Client-SDO service 存在并发起 cancellation；最终 completion callback 的 abort code 才是 transaction 结果判据。B02-08 在观察到 client abort completion 后，使用一次确认的 NMT Stop -> Pre-operational 状态切换作为远端 SDO readiness barrier，不使用固定 sleep，也不执行 communication reset。

B02-12 位于现有 `sdoClientProcess()` 内，因为该回归复用 J04/B03 的 `0x2303` MCU SDO Client 控制/status contract 和 Host Node-127 Server-SDO fixture。实际执行要求 `CANOPEN_ENABLE_SDO_CLIENT_PROCESS=1` 且 `CANOPEN_ENABLE_SDO_CLIENT_BLOCK_REGRESSION=1`；头文件对该依赖进行编译期检查。MCU 侧要求 `PKG_CANOPENNODE_DEMO_SDO_CLIENT_TEST=y` 和 `PKG_CANOPENNODE_SDO_CLI_BLOCK=y`；前者会选择基础 SDO Client/segmented/local 支持，后者单独编译 block 能力。`PKG_CANOPENNODE_APP_SDO_CLI_BLOCK` 不是 B02-12 必需项，因为该测试由 `0x2303:08` bit0 对每次 request 显式选择 block。J04/B03 原 `flags=0` segmented/local 路径保持不变。

## A03 RPDO/TPDO

声明位置：`include/pdo_process.h`。

```cpp
int pdoProcess(
    lely::canopen::AsyncMaster& master);
```

A03 使用 Lely 原生 PDO 能力：

- `OnRpdo()` 接收从机 TPDO1 对应的主站 RPDO1 callback；
- `RpdoMapped()`读取从机 TPDO 映射对象的主站本地镜像；
- `TpdoMapped()`更新从机 RPDO 映射对象并触发主站 TPDO1；
- `OnTpdo()`确认主站 PDO 实际发送结果。

A03 的 PDO number、OD index/subindex、probe value、采样数、周期容差和超时均位于 `src/pdo_process.cpp` 的匿名命名空间中。

## A05 TIME Consumer（当前默认关闭）

声明位置：`include/time_process.h`。

```cpp
int timeProcess(
    lely::canopen::AsyncMaster& master);
```

A05 使用公共远端 SDO helper 保存、修改和恢复从机 `0x1012:00`，并通过 Lely 内部 CAN network 发送可控 TIME 帧。MCU 必须提供只读诊断记录 `0x2300:01..03`，分别表示合法 DLC=6 TIME 接收计数、应用层 `CO_TIME_t::ms` 和 `CO_TIME_t::days`。主机通过接收计数确认帧到达，通过 `ms/days` 独立确认 TIME 是否真正被应用。当前 `CANOPEN_ENABLE_TIME_PROCESS=0`，因此 A05 不注册到默认 Master 流程；启用后 MCU `0x2300:01..03` diagnostic 作为自动断言契约。

禁用 consumer 时，CANopenNode 动态 `0x1012` 写入只改变 `isConsumer`，不会注销已经建立的 RX buffer；因此 A05 允许合法 TIME 接收计数继续增加，但要求 `ms/days` 只按已有时间自然推进，不能跳到测试时间戳。若测试从初始 consumer-disabled 状态启用 bit31，A05 会执行一次 Reset Communication，使 CANopenNode 按新 `0x1012` 重新建立 TIME RX buffer。

## 共享 EMCY observer

声明位置：`include/canopen_emcy.h`。

```cpp
struct CanopenEmcyEvent;

void registerCanopenEmcyCallback(
    lely::canopen::AsyncMaster& master);

std::uint64_t snapshotCanopenEmcySequence();

bool waitForCanopenEmcyEvent(...);

std::vector<CanopenEmcyEvent> getCanopenEmcyEventsAfter(...);
```

Lely `OnEmcy()` 同时只能注册一个函数，因此 `main.cpp` 只注册一次共享 callback。observer 为每条 EMCY 分配单调 sequence、记录 `steady_clock` 时间戳并保留最近 32 条事件；A01/A06 在触发动作前 snapshot sequence，只接受之后发布的事件，从而避免旧 callback 满足新断言。

## A06 EMCY Producer

声明位置：`include/emcy_process.h`。

```cpp
int emcyProcess(
    lely::canopen::AsyncMaster& master);
```

A06 不写 `0x1016`。它读取从机 `0x1001/0x1003/0x1014/0x1015`，基础 EMCY 阶段先临时把 `0x1015` 置 0 并回读以隔离 inhibit 影响，再以停止主站本地 `0x1017` 作为 EMCY fault source；动态 COB-ID 测试同时切换从机 `0x1014` 和主站本地 `0x1028:01`，且遵守 disable/change/enable 约束。测试值 `0x681` 和 `0x1015=15000` 仅存在于运行期，结束时恢复保存值并回读。

## B06 EMCY Consumer

声明位置：`include/emcy_consumer_process.h`。B06 Host local EMCY 所需的最小 Lely 扩展位于 `include/canopen_master.h`。

```cpp
int emcyConsumerProcess(CanopenTestMaster& master);

class CanopenTestMaster : public lely::canopen::AsyncMaster;
```

B06 不使用 Host 共享 `OnEmcy()` observer。Host Node 127 通过 `CanopenTestMaster::pushLocalEmcy()` 发送非零 EMCY，MCU 通过 CANopenNode EMCY Consumer callback 更新只读 `0x2301:01..07`。Host 以 `remote_rx_count` 前读/后读相等作为多次 SDO snapshot 的一致性条件，并逐帧要求 count 精确 `+1`。

`pushLocalEmcy()` 在 master 锁内调用现有 COEmcy::push() 并返回结果，只有确认 local EMCY 已入栈后才更新 host_error_active/expected_host_error_count。

Master process table 使用绑定后的 callable，因此普通流程仍接收 `AsyncMaster&`，B06 可直接接收 `CanopenTestMaster&`；B09G 额外绑定独立 `CanChannel&`。所有流程仍由同一个 `canopenRunProcesses()` 统一记录 started/passed/failed 并 fail-fast。

标准 recovery 不能通过 `AsyncMaster::Error(0)` 产生；`CanopenTestMaster` 中的 local EMCY access 是 B06-only，只在 Lely master 锁内暴露现有 local EMCY service 的 `pushLocalEmcy()/peekLocalEmcy()/clearLocalEmcy()`，其中 `clear()` 发送标准 `0x0000` error reset。该扩展不提供任意 CAN frame API。

B06 preflight 要求 Host local EMCY active stack 为空。生成的 compact `0x1003` 在 Lely EMCY service 第一次同步前可能仍保持 DCF 初始值，因此初始 `0x1003:00` 不作为 active stack 深度；但 B06 自己开始产生 EMCY 后，cleanup 前仍通过已经同步的 `0x1003:00`、栈顶和 combined Error Register 共同保护全栈 `clear()`。

B06 在 Reset Communication 前先建立一个非零 vector A persistence marker；Reset 后要求 `remote_rx_count` 与完整非零 EMCY snapshot 全部保持不变，随后 fresh vector B 必须再次精确 `+1`，用于同时证明 diagnostic 持久性和 MCU callback 已重新绑定。fresh Boot callback 的实际 NMT state 同时作为 reset 后状态证据：若已经为 `START`，不重复发送 NMT Start；否则显式 Start 并要求 fresh `OnState(START)`。

## J03 / B09G GFC

声明位置：`include/gfc_process.h`。

```cpp
int gfcProcess(
    CanopenTestMaster& master,
    lely::io::CanChannel& wire_channel);
```

B09G 不向 `AsyncMaster` 增加伪 GFC service。现有 master channel 继续由 Lely CANopen 协议层独占；`main.cpp` 在 Master role 下另外打开一个同接口的 `CanChannel`，只允许 `gfc_process.cpp` 私有 fixture 发送/捕获 CAN-ID `0x001`。fixture 仅支持合法 DLC0 与负向测试 DLC1，不提供通用任意帧 API。

MCU 通过标准 `0x1300:00` 控制 GFC valid，并用 test-only `0x2302:01..05` 发布 consumer count、安全请求标志以及 producer request/complete/result。Producer 请求由 SDO 写入 sequence，MCU mainline 调用 `CO_GFCsend()`；Host 必须同时等待 completion 并在独立 wire channel 上捕获标准 `0x001/DLC0`。

B09G 保存并恢复测试前 `0x1300`；Reset Communication 后要求 consumer count 保持不变、producer 无 pending request，并通过 fresh GFC 证明 callback 重绑。详细用例见 [`CANopen_GFC_Test.md`](CANopen_GFC_Test.md)。该阶段只声明 GFC protocol functional verification，不声明 SIL/PL/功能安全认证。

## NMT Master 行为验证（Slave role）

声明位置：`include/nmt_master_process.h`。

```cpp
int nmtMasterProcess(
    lely::canopen::BasicSlave& slave);
```

该流程只在 `CANOPEN_ROLE_SLAVE` 下由 `runCanopenSlave()` 调用，不进入 Master 的 A/B stage process table。Linux Node 2 通过 `BasicSlave::OnCommand()` 观察 MCU NMT Master 的正式命令、fixture PRE-OP 归一化命令和 Lely reset 内部状态迁移。callback 进入固定 FIFO 后由控制线程使用 `condition_variable` 有界消费，因此 reset completion、fixture auto START 与 MCU PREOP 连续到达时不会被 latest-state 覆盖。

Host 复用 MCU 提供的 `project.eds` 且不修改 NMT startup；Host 只读取本地 `0x1F80` startup bit 2 来确定 reset 后是否必须出现 fixture auto-start。MCU 自动测试通过 Heartbeat Consumer 发现 Node 2，并按该实际 startup 行为发送 PREOP 归一化后执行正式六步 NMT 序列。RESET_NODE 后若 auto-start 必须发生，Host 严格要求 `fixture START -> PREOP normalization -> distinct final START`，不再通过静默窗口猜测 START 来源。Host 不通过 SDO 或 Raw CAN 触发 MCU。最终 formal START callback 后 Host 继续保持 Node 2 Operational 两个 Producer Heartbeat 周期，随后才返回成功。完整序列和验证边界见 [`CANopen_NMT_Master_Test.md`](CANopen_NMT_Master_Test.md)。

## Final Reset

声明位置：`include/shutdown_process.h`。

```cpp
int finalResetProcess(
    lely::canopen::AsyncMaster& master);
```

该流程清除 Boot 等待状态，发送定向 Reset Communication，并等待目标节点新的 Boot callback。`Context::shutdown()`、线程回收和对象析构仍由 `main()` 负责。
