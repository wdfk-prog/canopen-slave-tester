# API 文档

本文档描述版本 `0.3.0` 的稳定外部接口：命令行、退出码、配置入口和 `include/canopen_test/` 下的 C++ 公共接口。当前工程不安装共享库；C++ API 主要供工程内部和后续测试 Harness 使用。

## 命令行接口

### 语法

```text
canopen_slave_tester [options]
```

### 选项

| 选项             | 说明                                                                                | 退出行为                       |
| ---------------- | ----------------------------------------------------------------------------------- | ------------------------------ |
| `--config PATH`  | 指定 `tester.conf`；默认 `/opt/Ultra/Debug/canopen-slave-tester/config/tester.conf` | 继续执行其他选项或启动运行时   |
| `--check-config` | 解析、校验配置和引用文件后退出                                                      | 成功打印 `configuration: PASS` |
| `--version`      | 打印工具、阶段、Lely 和 spdlog 版本                                                 | 立即退出 `0`                   |
| `-h`, `--help`   | 打印帮助                                                                            | 立即退出 `0`                   |

未知参数或缺少 `--config` 路径时抛出 usage error。

### 版本输出

```text
canopen_slave_tester=0.3.0
stage=P1-runtime
built_against_lely=2.4.0
spdlog=1.17.0
```

### 退出码

`canopen_test::ExitCode`：

| 枚举             |  值 | 含义                                          |
| ---------------- | --: | --------------------------------------------- |
| `kSuccess`       |   0 | 成功                                          |
| `kUsage`         |   1 | CLI 参数错误                                  |
| `kConfiguration` |   2 | 配置解析、范围、路径或文件错误                |
| `kSocketCan`     |   3 | CAN 接口、波特率、状态或 channel 错误         |
| `kLelyRuntime`   |   4 | DCF、Lely 初始化、event loop 或异步运行时错误 |
| `kInternal`      |   5 | 未分类 C++ 异常或未知异常                     |

## Application

头文件：`include/canopen_test/application.hpp`

```cpp
class Application final {
public:
    int Run(int argc, char* argv[]);
};
```

### `Application::Run`

职责：

1. 解析 CLI；
2. 加载 `AppConfig`；
3. 处理 `--check-config`；
4. 初始化日志；
5. 创建 `RuntimeStatus`、`LelyRuntime` 和 `SignalHandler`；
6. 调用 `Start()`、`Run()`、`Stop()`；
7. 输出最终 session 摘要。

返回稳定退出码。`ApplicationError` 和其他异常由 `main()` 顶层边界转换为 stderr 消息和退出码。

## AppConfig

头文件：`include/canopen_test/app_config.hpp`

```cpp
struct AppConfig {
    std::string source_path;
    std::string can_interface;
    std::uint32_t can_bitrate;
    std::uint8_t master_node_id;
    std::string master_dcf_path;
    std::uint8_t slave_node_id;
    std::string slave_eds_path;
    std::uint32_t boot_timeout_ms;
    std::uint32_t sdo_timeout_ms;
    std::uint32_t nmt_state_timeout_ms;
    std::uint32_t pdo_timeout_ms;
    std::uint32_t lss_timeout_ms;
    std::string history_path;
    std::string report_directory;
    LoggingConfig logging;

    static AppConfig Load(const std::string& path);
    std::string ToString() const;
};
```

### `AppConfig::Load`

```cpp
static AppConfig Load(const std::string& path);
```

- 输入：INI 配置路径；
- 输出：完全校验后的值快照；
- 相对路径以配置文件目录解析；
- DCF 必须为非空普通文件；EDS 配置为非空路径时也必须有效；
- 失败抛出 `ConfigError`。

该对象加载后按值传给运行时，不支持动态重载。

### `AppConfig::ToString`

返回无脱敏需求的多行启动配置快照。当前配置不包含密码或密钥；未来加入敏感字段时必须先修改该接口的输出策略。

## LelyRuntime

头文件：`include/canopen_test/lely_runtime.hpp`

```cpp
class LelyRuntime final {
public:
    explicit LelyRuntime(RuntimeStatus& status);
    ~LelyRuntime() noexcept;

    void Start(const AppConfig& config);
    int Run();
    void RequestStop() noexcept;
    void Stop() noexcept;
};
```

不可复制或赋值。

### 构造函数

```cpp
explicit LelyRuntime(RuntimeStatus& status);
```

`RuntimeStatus` 由调用者拥有，其生命周期必须覆盖 `LelyRuntime`。

### `Start`

```cpp
void Start(const AppConfig& config);
```

创建并启动完整运行时。事务语义：任一步失败都会释放已创建资源。

异常：

- `SocketCanError`：接口不存在、波特率不匹配、状态为 STOPPED/BUS-OFF、channel 打开失败；
- `LelyRuntimeError`：I/O context、DCF、`AsyncMaster` 或其他 Lely 初始化失败；
- 已启动时再次调用会抛 `LelyRuntimeError`。

成功后生命周期为 `kReady`。

### `Run`

```cpp
int Run();
```

在调用线程执行 `Loop::run()`，直到正常停止。成功返回 `0`。

异常条件：

- 未调用 `Start()`；
- event loop 已经运行；
- loop 抛异常或返回 error code；
- callback 记录了异步失败；
- 未收到 stop request 却意外停止。

### `RequestStop`

```cpp
void RequestStop() noexcept;
```

线程安全、幂等。把 shutdown task 提交到 event-loop executor，不在调用线程销毁资源。适用于 signal listener 或控制线程。

### `Stop`

```cpp
void Stop() noexcept;
```

线程安全、幂等。若 `Run()` 在其他线程活动，先停止 loop 并等待其退出，然后逆序释放资源。析构函数自动调用。

不能在 event-loop 线程上调用后立即销毁对象；该路径只请求停止，所有者必须等待 `Run()` 返回。

## RuntimeStatus

头文件：`include/canopen_test/runtime_status.hpp`

### 生命周期枚举

```cpp
enum class RuntimeLifecycle {
    kCreated,
    kStarting,
    kReady,
    kRunning,
    kStopping,
    kStopped,
    kFailed,
};
```

### CAN 状态枚举

```cpp
enum class RuntimeCanState {
    kUnknown,
    kActive,
    kPassive,
    kBusOff,
    kSleeping,
    kStopped,
};
```

### 状态快照

```cpp
struct RuntimeStatusSnapshot {
    std::uint64_t session_id;
    RuntimeLifecycle lifecycle;
    RuntimeCanState can_state;
    std::string last_event;
    std::string last_exception;
    std::uint64_t can_error_count;
};
```

### recorder 方法

```cpp
void ResetForStart();
void SetLifecycle(RuntimeLifecycle lifecycle);
void SetCanStateEvent(RuntimeCanState state, std::string event);
void RecordCanError(std::string event);
void SetLastEvent(std::string event);
void SetLastException(std::string exception);
RuntimeStatusSnapshot Snapshot() const;
```

所有方法通过内部 mutex 同步。调用者不能获取 mutex，也不应在持有其他运行时锁时执行长时间状态操作。

`ResetForStart()` 增加 `session_id` 并清空上一会话数据。`SetLastException()` 同时将生命周期标记为 failed。

## SignalHandler

头文件：`include/canopen_test/signal_handler.hpp`

```cpp
class SignalHandler final {
public:
    using Callback = std::function<void(int signal_number)>;
    explicit SignalHandler(Callback callback);
    ~SignalHandler() noexcept;
};
```

- 安装 `SIGINT` 和 `SIGTERM`；
- signal trampoline 只执行 async-signal-safe pipe write；
- callback 在内部 listener thread 执行；
- 同一进程同时只能存在一个活动实例；
- 空 callback 抛 `std::invalid_argument`；
- 重复实例抛 `std::logic_error`；
- pipe/sigaction/thread 失败抛 `std::system_error`。

## Logger

头文件：`include/canopen_test/logger.hpp`

### 日志等级

```cpp
enum class LogLevel {
    kTrace,
    kDebug,
    kInfo,
    kWarning,
    kError,
    kCritical,
    kOff,
};
```

### 日志类别

```cpp
enum class LogCategory {
    kApplication,
    kConfiguration,
    kRuntime,
    kSocketCan,
    kCanOpen,
    kSignal,
    kLely,
};
```

### LoggingConfig

包含每个 category 的最低等级、console/file sink 等级、flush 等级、队列大小、worker 数量、文件路径、轮转大小和数量。

### 函数

```cpp
void InitializeLogging(const LoggingConfig& config);
bool ShouldLog(LogLevel level, LogCategory category) noexcept;
void Log(LogLevel level, LogCategory category,
         const std::string& message) noexcept;
void ShutdownLogging() noexcept;
std::uint64_t DroppedLogCount() noexcept;
const char* ToString(LogLevel level) noexcept;
const char* ToString(LogCategory category) noexcept;
```

`Log()` 不向调用者传播异常。日志未初始化或已关闭时，warning 及以上回退到 stderr。

## 错误类型

头文件：`include/canopen_test/error.hpp`

```cpp
class ApplicationError : public std::runtime_error {
public:
    ExitCode code() const noexcept;
    const std::string& category() const noexcept;
};

class ConfigError final : public ApplicationError {};
class SocketCanError final : public ApplicationError {};
class LelyRuntimeError final : public ApplicationError {};
```

应用层应优先抛最具体类型，保留稳定类别和退出码。`noexcept` callback/control path 不应抛这些异常，而应记录状态并停止 loop。

## 版本接口

头文件：`include/canopen_test/version.hpp`

```cpp
const char* TesterVersion() noexcept;
const char* BuiltAgainstLelyVersion() noexcept;
const char* SpdlogVersion() noexcept;
```

返回编译期静态字符串。Lely 版本来自 staged `liblely-coapp.pc`，spdlog 版本来自 vendored `version.h`。

## 兼容性约束

- C++14；
- Linux/POSIX；
- Lely 2.4.0 当前已验证；
- public header 隔离了大部分 Lely 类型，`LelyRuntime` 使用 PImpl；
- API 尚未声明 ABI 稳定性；在工程进入库化阶段前，应按源码级接口管理。
