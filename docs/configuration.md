# Heartbeat 配置与生成

## 配置来源

双方 Heartbeat 配置统一由 `config/master.yml` 描述。节点 1 的 `0x1016/0x1017` 由 `dcfgen` 写入 `mcu_node_1.bin`，Lely 在 Boot 流程中自动下载，不在应用运行期重复写入。

关键配置：

```yaml
options:
  dcf_path: "/opt/Ultra/Debug/canopen-master/config"
  heartbeat_multiplier: 3.0

master:
  node_id: 127
  baudrate: 1000
  heartbeat_consumer: true
  heartbeat_producer: 500
  start: false
  start_nodes: false
  start_all_nodes: false
  reset_all_nodes: false
  stop_all_nodes: false
  boot_time: 0

mcu_node_1:
  dcf: "generated/project.dcfgen.eds"
  node_id: 1
  heartbeat_consumer: true
  heartbeat_producer: 500
  heartbeat_multiplier: 3.0
  retry_factor: 0
  boot: true
  mandatory: false
  reset_communication: true
```

由此得到：

| 方向 | Producer | Consumer | 超时 |
| --- | --- | --- | ---: |
| 主站发送、从机消费 | 主站 Node-ID 127，500 ms | 从机监控 127 | 1500 ms |
| 从机发送、主站消费 | 从机 Node-ID 1，500 ms | 主站监控 1 | 1500 ms |

## 生成命令

```sh
cd config

../.venv-dcf-tools/bin/dcfgen \
    -r \
    -v \
    -d generated \
    master.yml
```

生成目录必须包含：

```text
generated/master.dcf
generated/master.bin
generated/mcu_node_1.bin
```

当前主站没有启动期 concise DCF 写条目，因此 `master.bin` 是零条目兼容文件，当前运行流程不引用它。若所用 `dcfgen` 版本不主动重写该文件，必须保留目录中已有的兼容文件。

详细日志应包含：

```text
mcu_node_1: writing 4 bytes to 0x1016/1: DC 05 7F 00
mcu_node_1: writing 2 bytes to 0x1017/0: F4 01
```

其中：

```text
0x007F05DC = Node-ID 127 << 16 | 1500 ms
0x01F4     = 500 ms
```

## master.dcf 检查

```sh
grep -A4 '^\[1016Value\]' generated/master.dcf
grep -A4 '^\[1F81Value\]' generated/master.dcf
grep -n 'mcu_node_1.bin' generated/master.dcf
```

要求：

```ini
[1016Value]
NrOfEntries=1
1=0x000105DC
```

`0x1F81:01` 必须为 `0x00000005`：节点分配有效、启用 Boot，且 bit 4 清零表示 `reset_communication: true`。`0x1F22:01` 的 `UploadFile` 必须指向：

```text
/opt/Ultra/Debug/canopen-master/config/mcu_node_1.bin
```

## concise DCF 二进制结构

`mcu_node_1.bin` 使用 little-endian concise DCF 格式：

```text
02 00 00 00                         entry count = 2
16 10 01 04 00 00 00 DC 05 7F 00   0x1016:01, 4 bytes
17 10 00 02 00 00 00 F4 01         0x1017:00, 2 bytes
```

## 编译期配置

`include/canopen_config.h` 只保存被多个模块或主运行时共同使用的全局配置。协议专属配置和单个流程的开关必须放在对应模块头文件，避免公共配置头持续膨胀。

全局配置：

| 宏 | 默认值 | 说明 |
| --- | ---: | --- |
| `CANOPEN_INTERFACE_NAME` | `"can1"` | SocketCAN 接口 |
| `CANOPEN_EXPECTED_BITRATE` | `1000000` | 期望 bitrate，bit/s |
| `CANOPEN_MASTER_NODE_ID` | `127` | 主站 Node-ID |
| `CANOPEN_SLAVE_NODE_ID` | `1` | 被测从机 Node-ID |
| `CANOPEN_MASTER_DCF_PATH` | `../config/master.dcf` | 相对远端 `bin/` 的 DCF 路径 |
| `CANOPEN_CHANNEL_RX_QUEUE_SIZE` | `256` | SocketCAN receive queue |
| `CANOPEN_WAIT_TIMEOUT_MS` | `5000` | Startup Boot、A04 异常恢复和 Final Reset 等跨流程等待时间 |
| `CANOPEN_LOG_QUEUE_SIZE` | `8192` | 异步日志队列容量 |
| `CANOPEN_LOG_WORKER_COUNT` | `1` | 异步日志 worker 数量 |

模块专属配置：

| 头文件 | 宏 | 默认值 | 说明 |
| --- | --- | ---: | --- |
| `include/canopen_sdo.h` | `CANOPEN_SDO_TIMEOUT_MS` | `5000` | 公共远端 SDO helper 传给 Lely 的协议级 timeout |
| `include/canopen_sdo.h` | `CANOPEN_SDO_COMPLETION_MARGIN_MS` | `500` | SDO 协议 timeout 后额外等待 completion callback 的本地余量 |
| `include/nmt_heartbeat.h` | `CANOPEN_ENABLE_HEARTBEAT_PROCESS` | `1` | 注册并执行 A01 Heartbeat |
| `include/sdo_process.h` | `CANOPEN_ENABLE_SDO_PROCESS` | `1` | 注册并执行 A02 SDO |
| `include/pdo_process.h` | `CANOPEN_ENABLE_PDO_PROCESS` | `1` | 注册并执行 A03 RPDO/TPDO |
| `include/sync_pdo_process.h` | `CANOPEN_ENABLE_SYNC_PDO_PROCESS` | `1` | 注册并执行 A04 SYNC/同步 TPDO |
| `include/time_process.h` | `CANOPEN_ENABLE_TIME_PROCESS` | `0` | A05 主机侧已实现；MCU `0x2300:01..03` 就绪后再注册 TIME Consumer 测试 |
| `include/shutdown_process.h` | `CANOPEN_ENABLE_FINAL_RESET_PROCESS` | `1` | 退出时执行 Reset Communication |

测试流程自己的对象索引、probe value、采样数量、周期容差和流程 timeout 不放入公共配置头。当前分别位于：

```text
A01  src/nmt_heartbeat.cpp
A02  src/sdo_process.cpp
A03  src/pdo_process.cpp
A04  src/sync_pdo_process.cpp
A05  src/time_process.cpp
```

A01 当前使用 `kHeartbeatIndex=0x1017`、`kHeartbeatTimeoutMs=3000`、`kHeartbeatSampleCount=5` 和 500 ms Producer Heartbeat 周期。A02 使用 `kTestObjectIndex=0x2200`、`kProbeValue=0x12345678`，并在 probe 与原值相同时选择备用值。A03 的 PDO number、`0x2100/0x2101/0x2200`、采样策略和时序容差全部保存在 `pdo_process.cpp`；A04 的 SYNC 周期、样本数量、TPDO1 参数索引和时序窗口保存在 `sync_pdo_process.cpp`；A05 的 `0x1012/0x2300`、TIME 边界值、诊断轮询和时序容差保存在 `time_process.cpp`。

从机的基线 Heartbeat 和 PDO 参数仍由 YAML、EDS、`dcfgen` 和 Lely Boot 管理。修改真实通信参数时必须同步修改配置源并重新生成 DCF；源码中的流程私有常量只定义测试行为。

### 自动流程开关

自动验证流程由 `main.cpp` 中的静态注册表按顺序执行：

```text
A01 Heartbeat
A02 SDO
A03 PDO
A04 SYNC PDO
A05 TIME  (当前宏默认 0，不注册)
```

各 `CANOPEN_ENABLE_*_PROCESS` 宏位于对应流程头文件；值为 `1` 时进入注册表，为 `0` 时不注册。流程采用 fail-fast：任一流程返回非零后，后续流程不再执行。

`CANOPEN_ENABLE_FINAL_RESET_PROCESS` 位于 `include/shutdown_process.h`，只控制退出阶段的 Reset Communication，不属于自动验证流程表。

## 部署

`deploy/run.sh` 默认上传：

```text
build/canopen_master                  -> bin/canopen_master
config/generated/master.dcf          -> config/master.dcf
config/project.eds                    -> config/project.eds
config/generated/mcu_node_1.bin      -> config/mcu_node_1.bin
```

可使用以下环境变量覆盖本地路径：

```text
CANOPEN_MASTER_DCF_PATH
CANOPEN_PROJECT_EDS_PATH
CANOPEN_MCU_NODE_DCF_PATH
```
