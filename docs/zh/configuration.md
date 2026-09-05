[English](../en/configuration.md)

# 配置说明

## 共享运行配置

`include/canopen_config.h` 只保存跨模块共享配置：

| 配置 | 当前值 | 含义 |
| --- | ---: | --- |
| `CANOPEN_ROLE` | `CANOPEN_ROLE_MASTER` | Host 运行角色 |
| `CANOPEN_INTERFACE_NAME` | `"can1"` | SocketCAN 设备 |
| `CANOPEN_EXPECTED_BITRATE` | `1000000` | 期望 nominal bitrate，bit/s |
| `CANOPEN_MASTER_NODE_ID` | `127` | Lely Master Node-ID |
| `CANOPEN_SLAVE_NODE_ID` | `1` | MCU Node-ID |
| `CANOPEN_PEER_PROFILE` | `CANOPEN_PEER_PROFILE_LELY_RTT_B4` | 选择 BasicSlave 测试夹具契约 |
| `CANOPEN_PEER_NODE_ID` | 默认 profile 为 `1` | Lely `BasicSlave` peer Node-ID |
| `CANOPEN_PEER_HEARTBEAT_MS` | 默认 profile 为 `1000` | 软件 peer Heartbeat 周期 |
| `CANOPEN_WAIT_TIMEOUT_MS` | `5000` | 公共 Boot/NMT wait timeout |
| `CANOPEN_CHANNEL_RX_QUEUE_SIZE` | `256` | Lely CAN RX queue |
| `CANOPEN_LOG_QUEUE_SIZE` | `8192` | spdlog async queue |
| `CANOPEN_LOG_WORKER_COUNT` | `1` | spdlog worker 数量 |

## BasicSlave peer profiles

`CANOPEN_ROLE_SLAVE` 通过 `CANOPEN_PEER_PROFILE` 选择两种测试夹具：

- `CANOPEN_PEER_PROFILE_LELY_RTT_B4`（默认）：Node 1，加载 `config/lely_rtt_node1.dcf`，Heartbeat 1000 ms，只被动记录 NMT transition callback，不要求固定命令顺序；用于 `lely-canopen-rtt` B4 Master 集成/HIL。
- `CANOPEN_PEER_PROFILE_CANOPENNODE_NMT`：Node 2，加载 `config/project.eds`，Heartbeat 500 ms，保留历史 `nmtMasterProcess()` 严格序列；使用该 profile 后才可把 `CANOPEN_ENABLE_NMT_MASTER_PROCESS` 设为 `1`。

Lely B4 profile 必须保持 `CANOPEN_ENABLE_NMT_MASTER_PROCESS=0`，否则 NMT boot 自动 Reset Communication 会与历史固定序列断言冲突，产生测试夹具假失败。

## 协议验证开关

每个协议模块的 enable 宏保留在自己的接口头文件中。当前仓库值为：

| 能力 | 宏 | 默认值 |
| --- | --- | ---: |
| Heartbeat | `CANOPEN_ENABLE_HEARTBEAT_PROCESS` | `0` |
| SDO object access | `CANOPEN_ENABLE_SDO_PROCESS` | `0` |
| SDO server block transfer | `CANOPEN_ENABLE_SDO_BLOCK_PROCESS` | `0` |
| Storage persistence | `CANOPEN_ENABLE_STORAGE_PROCESS` | `0` |
| MCU SDO client | `CANOPEN_ENABLE_SDO_CLIENT_PROCESS` | `0` |
| MCU SDO client block regression | `CANOPEN_ENABLE_SDO_CLIENT_BLOCK_REGRESSION` | `0` |
| PDO | `CANOPEN_ENABLE_PDO_PROCESS` | `0` |
| SYNC / synchronous PDO | `CANOPEN_ENABLE_SYNC_PDO_PROCESS` | `0` |
| TIME consumer | `CANOPEN_ENABLE_TIME_PROCESS` | `0` |
| EMCY producer | `CANOPEN_ENABLE_EMCY_PROCESS` | `0` |
| EMCY consumer | `CANOPEN_ENABLE_EMCY_CONSUMER_PROCESS` | `0` |
| GFC | `CANOPEN_ENABLE_GFC_PROCESS` | `0` |
| SRDO | `CANOPEN_ENABLE_SRDO_PROCESS` | `1` |
| NMT Master behavior | `CANOPEN_ENABLE_NMT_MASTER_PROCESS` | `0` |
| Final Reset Communication | `CANOPEN_ENABLE_FINAL_RESET_PROCESS` | `1` |

不能因为 Host 端存在实现就直接打开流程；必须先确认 MCU CANopenNode 配置和对应 diagnostic OD 已准备好。

## Lely DCF/EDS 配置

`config/master.yml` 定义 `dcfgen` 使用的 Lely Master 和 MCU 节点。当前基线包括：

- Master Node-ID `127`；
- MCU Node-ID `1`；
- bitrate `1000` kbit/s；
- Master 与 MCU Producer Heartbeat `500` ms；
- 双向 Heartbeat Consumer；
- Master 自动 Start 关闭；
- MCU boot/configuration 开启，并允许 Reset Communication。

生成结果位于 `config/generated/`。只要 EDS 或基线通信配置发生变化，就应重新生成并复核 DCF。

## TQ8MP 交叉编译配置

默认构建加载 `cmake/build_config.cmake`，它要求存在开发机私有的 `cmake/build_config.local.cmake`。该本地文件提供 Yocto toolchain、sysroot、Lely include/library 路径和可选目标板地址。

机器路径与凭据不应提交。本地构建和部署配置已加入 `.gitignore`；如果已有副本被 Git 跟踪，还需要单独从 Git index 中移除。

## 可选 Native Host 构建

只有明确需要本机 native Linux 构建且存在兼容 Lely 安装时才使用 `CANOPEN_NATIVE_BUILD=ON`。CI 不执行目标交叉编译；Release workflow 使用真实 TQ8MP 交叉工具链。

必须显式提供：

```text
CANOPEN_LELY_INCLUDE_DIR
CANOPEN_LELY_LIBRARY_DIR
```

该可选 Host 构建与 TQ8MP 产品构建隔离，不能作为 Release 构建证据。
