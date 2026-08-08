# 代码接口

当前工程不提供命令行参数、共享库 ABI 或第三方 C++ SDK。公开头文件只声明工程内部流程接口。

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

模块注册 `OnBoot()`、`OnHeartbeat()` 和 `OnEmcy()`，并在内部维护 Boot、Heartbeat 和 EMCY 的等待状态。A01 流程私有的对象索引、等待时间、采样数量和 Producer Heartbeat 周期位于 `src/nmt_heartbeat.cpp`，不暴露为公共宏。

## A02 SDO

声明位置：`include/sdo_process.h`。

```cpp
int sdoProcess(
    lely::canopen::AsyncMaster& master);
```

A02 保存节点 1 的 `0x2200:00`，写入临时 probe value，通过 SDO 回读验证后恢复原值。测试对象和 probe value 是 `src/sdo_process.cpp` 的私有配置。

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
