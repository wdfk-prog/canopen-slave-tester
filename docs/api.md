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

int heartbeatProcess(
    lely::canopen::AsyncMaster& master);
```

模块只注册 `OnBoot()` 和 `OnHeartbeat()`；EMCY 由共享 observer 注册并通过 sequence-based wait 提供给 A01。`CANOPEN_ENABLE_HEARTBEAT_PROCESS` 作为 A01 专属注册开关位于 `include/nmt_heartbeat.h`；对象索引、等待时间、采样数量和 Producer Heartbeat 周期位于 `src/nmt_heartbeat.cpp`，不暴露为公共宏。

## A02 SDO

声明位置：`include/sdo_process.h`。

```cpp
int sdoProcess(
    lely::canopen::AsyncMaster& master);
```

A02 保存节点 1 的 `0x2200:00`，写入临时 probe value，通过 SDO 回读验证后恢复原值。`CANOPEN_ENABLE_SDO_PROCESS` 位于 `include/sdo_process.h`；测试对象和 probe value 是 `src/sdo_process.cpp` 的私有配置。

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

## A05 TIME Consumer（主机侧已实现，默认关闭）

声明位置：`include/time_process.h`。

```cpp
int timeProcess(
    lely::canopen::AsyncMaster& master);
```

A05 使用公共远端 SDO helper 保存、修改和恢复从机 `0x1012:00`，并通过 Lely 内部 CAN network 发送可控 TIME 帧。MCU 必须提供只读诊断记录 `0x2300:01..03`，分别表示合法 DLC=6 TIME 接收计数、应用层 `CO_TIME_t::ms` 和 `CO_TIME_t::days`。主机通过接收计数确认帧到达，通过 `ms/days` 独立确认 TIME 是否真正被应用。当前 `CANOPEN_ENABLE_TIME_PROCESS=0`，在 MCU 诊断对象落地前不注册 A05。

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

## NMT Master 行为验证（Slave role）

声明位置：`include/nmt_master_process.h`。

```cpp
int nmtMasterProcess(
    lely::canopen::BasicSlave& slave);
```

该流程只在 `CANOPEN_ROLE_SLAVE` 下由 `runCanopenSlave()` 调用，不进入 Master 的 A01～A06 process table。Linux Node 2 通过 `BasicSlave::OnCommand()` 观察 MCU NMT Master 的正式命令、fixture PRE-OP 归一化命令和 Lely reset 内部状态迁移。callback 进入固定 FIFO 后由控制线程使用 `condition_variable` 有界消费，因此 reset completion、fixture auto START 与 MCU PREOP 连续到达时不会被 latest-state 覆盖。

Host 复用 MCU 提供的 `project.eds` 且不修改 NMT startup；Host 只读取本地 `0x1F80` startup bit 2 来确定 reset 后是否必须出现 fixture auto-start。MCU 自动测试通过 Heartbeat Consumer 发现 Node 2，并按该实际 startup 行为发送 PREOP 归一化后执行正式六步 NMT 序列。RESET_NODE 后若 auto-start 必须发生，Host 严格要求 `fixture START -> PREOP normalization -> distinct final START`，不再通过静默窗口猜测 START 来源。Host 不通过 SDO 或 Raw CAN 触发 MCU。最终 formal START callback 后 Host 继续保持 Node 2 Operational 两个 Producer Heartbeat 周期，随后才返回成功。完整序列和验证边界见 [`CANopen_NMT_Master_Test.md`](CANopen_NMT_Master_Test.md)。

## Final Reset

声明位置：`include/shutdown_process.h`。

```cpp
int finalResetProcess(
    lely::canopen::AsyncMaster& master);
```

该流程清除 Boot 等待状态，发送定向 Reset Communication，并等待目标节点新的 Boot callback。`Context::shutdown()`、线程回收和对象析构仍由 `main()` 负责。
