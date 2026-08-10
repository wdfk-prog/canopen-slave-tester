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

模块注册 `OnBoot()`、`OnHeartbeat()` 和 `OnEmcy()`，并在内部维护 Boot、Heartbeat 和 EMCY 的等待状态。`CANOPEN_ENABLE_HEARTBEAT_PROCESS` 作为 A01 专属注册开关位于 `include/nmt_heartbeat.h`；对象索引、等待时间、采样数量和 Producer Heartbeat 周期位于 `src/nmt_heartbeat.cpp`，不暴露为公共宏。

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

## Final Reset

声明位置：`include/shutdown_process.h`。

```cpp
int finalResetProcess(
    lely::canopen::AsyncMaster& master);
```

该流程清除 Boot 等待状态，发送定向 Reset Communication，并等待目标节点新的 Boot callback。`Context::shutdown()`、线程回收和对象析构仍由 `main()` 负责。
