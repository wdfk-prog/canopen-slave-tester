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

运行参数仍位于 `include/canopen_config.h`：

| 宏 | 默认值 | 说明 |
| --- | ---: | --- |
| `CANOPEN_INTERFACE_NAME` | `"can1"` | SocketCAN 接口 |
| `CANOPEN_EXPECTED_BITRATE` | `1000000` | 期望 bitrate，bit/s |
| `CANOPEN_MASTER_NODE_ID` | `127` | 主站 Node-ID |
| `CANOPEN_SLAVE_NODE_ID` | `1` | 被测从机 Node-ID |
| `CANOPEN_MASTER_DCF_PATH` | `../config/master.dcf` | 相对远端 `bin/` 的 DCF 路径 |
| `CANOPEN_WAIT_TIMEOUT_MS` | `5000` | Boot、EMCY、Heartbeat callback 或 SDO completion 最大等待时间 |
| `CANOPEN_HEARTBEAT_PERIOD_MS` | `500` | 双方 Producer 周期 |
| `CANOPEN_HEARTBEAT_MULTIPLIER` | `3` | Consumer 超时倍率 |

从机的基线 Heartbeat 配置仍由 YAML、`dcfgen` 和 Lely Boot 管理。自动专项测试会临时使用 `SubmitWrite()` 将节点 1 的 `0x1017:00` 写为 0/`CANOPEN_HEARTBEAT_PERIOD_MS`，但不改变持久配置；Heartbeat 周期或倍率变化后仍必须同步更新 YAML、重新生成 DCF、构建并部署。

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
