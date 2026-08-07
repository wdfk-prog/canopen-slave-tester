# 代码接口

当前工程不提供命令行参数、共享库ABI或第三方C++ SDK。头文件只声明工程内部流程。

## Heartbeat

声明位置：`include/nmt_heartbeat.h`。

```cpp
void registerNmtHeartbeatCallbacks(
    lely::canopen::AsyncMaster& master);

void prepareBootWait();

bool waitForBootCompletion(
    std::chrono::milliseconds timeout);

int heartbeatProcess(
    lely::canopen::AsyncMaster& master,
    lely::canopen::BasicDriver& driver);

void setRuntimeHeartbeatSupervision(bool enabled);

int processRuntimeHeartbeatReconnect(
    lely::canopen::BasicDriver& driver);
```

模块内部拥有Boot和NMT condition variable，以及runtime Boot pending原子标志。Heartbeat配置直接使用Lely API，不暴露RemoteXXX读写包装或配置快照结构。

`registerNmtHeartbeatCallbacks()`同时注册：

- `OnBoot()`；
- `OnState()`；
- `OnHeartbeat()`；
- `OnEmcy()`，仅用于日志。

## Final Reset

声明位置：`include/shutdown_process.h`。

```cpp
int finalResetProcess(
    lely::canopen::AsyncMaster& master);
```

该流程清除Boot等待状态，发送定向Reset Communication，并等待目标节点新的Boot callback。`Context::shutdown()`、线程回收和对象析构仍由`main()`负责。
