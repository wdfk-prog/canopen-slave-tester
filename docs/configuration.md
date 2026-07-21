# 配置 tester.conf 和 master.yml

工程有两层配置：

1. `config/tester.conf` 控制可执行程序如何打开 SocketCAN、加载文件、设置超时和日志；
2. `config/master.yml` 是 `dcfgen` 输入，决定生成的 `master.dcf` 中 CANopen 对象字典、NMT 启动策略和从机配置关系。

修改 `master.yml` 不会自动影响目标板。必须重新运行 `dcfgen`，再部署生成的 `master.dcf`。

## 配置 tester.conf

### 必填字段

| 字段                    | 范围/单位            | 说明                                    |
| ----------------------- | -------------------- | --------------------------------------- |
| `can.interface`         | 1～15 个受支持字符   | SocketCAN 接口，例如 `can1`             |
| `can.bitrate`           | 10000～1000000 bit/s | 必须与 Linux 接口实际标称波特率一致     |
| `master.node_id`        | 1～127               | 主站 Node-ID                            |
| `master.dcf_path`       | 非空文件             | Lely master DCF                         |
| `slave.node_id`         | 1～127               | 被测从机 Node-ID，不能与主站相同        |
| `timeouts.boot_ms`      | 1～600000 ms         | boot-up 等待超时，P1 仅加载保留         |
| `timeouts.sdo_ms`       | 1～600000 ms         | 设置到 Lely `AsyncMaster::SetTimeout()` |
| `timeouts.nmt_state_ms` | 1～600000 ms         | 后续 NMT Harness 保留                   |
| `timeouts.pdo_ms`       | 1～600000 ms         | 后续 PDO Harness 保留                   |
| `timeouts.lss_ms`       | 1～600000 ms         | 后续 LSS Harness 保留                   |

`slave.eds_path` 在实现中允许为空；配置后必须指向非空普通文件。

### 路径解析

相对路径以 `tester.conf` 所在目录为基准。例如：

```ini
[master]
dcf_path=master.dcf
```

若配置文件为 `/opt/app/config/tester.conf`，最终路径是 `/opt/app/config/master.dcf`。

以下路径使用相同规则：

- `master.dcf_path`
- `slave.eds_path`
- `console.history_path`
- `console.report_directory`
- `logging.file_path`

### 日志字段

| 字段                         |             默认值 | 约束                                          |
| ---------------------------- | -----------------: | --------------------------------------------- |
| `logging.*_level`            |      多数为 `info` | `trace/debug/info/warning/error/critical/off` |
| `logging.console_level`      |             `info` | 控制终端 sink                                 |
| `logging.file_level`         |            `debug` | 控制轮转文件 sink                             |
| `logging.flush_level`        |            `error` | 达到该等级时请求 flush                        |
| `logging.async_queue_size`   |             `4096` | 64～1048576                                   |
| `logging.async_worker_count` |                `1` | 1～8                                          |
| `logging.overflow_policy`    |   `overrun_oldest` | 当前只接受该值                                |
| `logging.file_enabled`       |             `true` | `true/false`                                  |
| `logging.file_path`          | `logs/runtime.log` | 启用文件日志时不能为空                        |
| `logging.file_max_size`      |          `5242880` | 1024～1073741824 byte                         |
| `logging.file_count`         |                `3` | 1～1000                                       |

`overrun_oldest` 保证 callback 不等待日志 I/O，但队列满时会覆盖最旧记录。退出摘要中的 `dropped_logs` 必须作为验收指标。

### 配置校验

目标板运行：

```sh
./bin/canopen_slave_tester --config config/tester.conf --check-config
```

成功输出：

```text
configuration: PASS
```

解析器会拒绝未知键、重复键、缺少必填键、格式错误、超范围数值和空 DCF/EDS。

## 配置 master.yml

### 文件角色

`master.yml` 不是程序运行时输入。`dcfgen` 使用它和从机 EDS 生成：

```text
config/generated/master.dcf
config/generated/master.bin
config/generated/project.dcfgen.eds
```

程序在目标板实际加载的是部署后的：

```text
/opt/Ultra/Debug/canopen-slave-tester/config/master.dcf
```

### P1 推荐基线

```yaml
master:
  node_id: 127
  baudrate: 1000
  sync_period: 0
  sync_window: 0
  sync_overflow: 0
  heartbeat_consumer: false
  heartbeat_producer: 0
  start: false
  start_nodes: false
  start_all_nodes: false
  reset_all_nodes: false
  stop_all_nodes: false
  boot_time: 0

mcu_node_1:
  dcf: "generated/project.dcfgen.eds"
  node_id: 1
  heartbeat_consumer: false
  heartbeat_producer: 0
  retry_factor: 0
  boot: false
  mandatory: false
  reset_communication: true
```

这套配置保留测试程序对 NMT 状态变化的控制，不自动进入 Operational，也不自动启动从机。

### 使用单帧广播复位

当前 P1 必须使用：

```yaml
reset_communication: true
```

生成的 `0x1F81:01` 应为：

```ini
1=0x00000001
```

Lely 启动时可以发送一帧：

```text
NMT: sending command specifier 130 to node 0
```

总线报文：

```text
000#8200
```

不要在当前拓扑中设置：

```yaml
reset_communication: false
```

该设置会在 `0x1F81:01` 中加入避免复位位，典型生成值为：

```ini
1=0x00000011
```

主站不能广播复位全部节点，因为广播会包含 Node-ID 1。Lely 因而可能逐个复位其他可能节点：

```text
82 02
82 03
...
82 7E
```

共 125 帧。所有 NMT 命令都使用 COB-ID `0x000`，MCU CAN 驱动会先接收，再由协议栈根据第二个字节判断目标 Node-ID。开启逐帧 trace 时，这种启动突发可能使 RT-Thread 软件接收队列溢出并上报 `EMCY 0x8110`。

### NMT 字段含义

| 字段                             |  当前值 | 作用                                 |
| -------------------------------- | ------: | ------------------------------------ |
| `master.start`                   | `false` | 主站不自动进入 Operational           |
| `master.start_nodes`             | `false` | 不自动启动已配置从机                 |
| `master.start_all_nodes`         | `false` | 不广播 NMT Start                     |
| `master.reset_all_nodes`         | `false` | mandatory 从机失败时不自动 reset all |
| `master.stop_all_nodes`          | `false` | mandatory 从机失败时不自动 stop all  |
| `mcu_node_1.boot`                | `false` | 不在启动阶段自动下发从机配置         |
| `mcu_node_1.mandatory`           | `false` | 从机失败不触发 mandatory 网络策略    |
| `mcu_node_1.reset_communication` |  `true` | 允许启动时广播 Reset Communication   |

`start_nodes:false` 只控制 NMT Start，不等于禁止 Reset Communication。

### Heartbeat

主站生产 heartbeat：

```yaml
master:
  heartbeat_producer: 1000
```

让主站监控从机 heartbeat：

```yaml
master:
  heartbeat_consumer: true

mcu_node_1:
  heartbeat_producer: 1000
```

启用前必须确认从机 EDS 支持并允许写入 `0x1017`。P1 基线关闭自动 heartbeat 配置，后续测试阶段按测试用例显式开启。

### PDO 覆盖

`master.yml` 中保留了 RPDO/TPDO 示例。启用前确认：

- PDO 编号存在于 EDS；
- 映射对象允许 PDO mapping；
- Classic CAN 单个 PDO 总长度不超过 64 bit；
- COB-ID 不冲突；
- 同步传输时已配置 SYNC；
- 不把临时测试值写入配置期 `sdo` 列表。

### 已知限制

模板中标注的 Lely dcf-tools 2.4.0 已知问题未完成本地修复和回归前，不应启用：

- 从机 `time_cob_id`；
- RPDO `event_deadline`；
- 显式 `configuration_file` 解析路径。

## 运行 dcfgen

首次创建虚拟环境、从 Lely 源码或 wheel 安装 dcf-tools、修复 `ModuleNotFoundError`，以及把生成结果部署到目标板，见 [dcfgen 安装与使用](dcfgen-setup.md)。

### 使用虚拟环境

从工程根目录：

```sh
source .venv-dcf-tools/bin/activate
cd config
dcfgen -r -v -d generated master.yml
cd ..
deactivate
```

### 直接调用虚拟环境程序

```sh
cd config
../.venv-dcf-tools/bin/dcfgen -r -v -d generated master.yml
cd ..
```

参数：

| 参数           | 作用                            |
| -------------- | ------------------------------- |
| `-r`           | 生成远程 PDO 映射信息           |
| `-v`           | 输出详细生成过程和 SDO 配置请求 |
| `-d generated` | 将输出写入 `config/generated/`  |
| `master.yml`   | 网络配置源文件                  |

从 `config` 目录运行很重要，因为 `dcf: "generated/project.dcfgen.eds"` 按当前工作目录解析。

## 检查生成结果

```sh
ls -lh config/generated
grep -n -A5 '^\[1F80\]' config/generated/master.dcf
grep -n -A5 '^\[1F81Value\]' config/generated/master.dcf
sha256sum config/generated/master.dcf
```

P1 基线：

```ini
[1F80]
DefaultValue=0x0000000D

[1F81Value]
NrOfEntries=1
1=0x00000001
```

部署后还应在目标板确认：

```sh
sha256sum /opt/Ultra/Debug/canopen-slave-tester/config/master.dcf
```

与本地生成文件一致后再启动程序。

## 增加从机

复制一个新的顶层 section，并使用唯一 Node-ID：

```yaml
mcu_node_2:
  dcf: "generated/another_device.eds"
  node_id: 2
  boot: false
  mandatory: false
  reset_communication: true
```

然后重新运行 `dcfgen`。同时更新 `tester.conf`、自动测试 Harness 和验收拓扑说明；当前 P1 程序只显式保存一个 `slave.node_id`，多从机自动测试属于后续阶段。
