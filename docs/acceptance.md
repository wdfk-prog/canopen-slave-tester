# P0 和 P1 验收结果与证据

本文只记录当前版本已经执行的验收结果、原始证据和未闭环项，不提供验收操作教程。

## 验收对象

| 项目     | 当前值                             |
| -------- | ---------------------------------- |
| 软件     | `canopen_slave_tester 0.3.0`       |
| 阶段     | `P1-runtime`                       |
| Lely     | `2.4.0`                            |
| spdlog   | `1.17.0`                           |
| 目标板   | TQ8MP，Linux/aarch64               |
| CAN 接口 | `can1`，1 Mbit/s                   |
| 主站     | Node-ID 127                        |
| 从机     | RT-Thread + CANopenNode，Node-ID 1 |
| 证据日期 | 2026-07-21                         |

## 总体结论

| 阶段        | 结论           | 说明                                                                                                                                         |
| ----------- | -------------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| P0 工程基础 | **通过**       | 目标板可执行文件、运行库、配置和 DCF 已能被实际加载并启动 Lely runtime。构建日志、失败退出码矩阵和本地/目标板 SHA-256 对照未随本轮日志归档。 |
| P1 运行时   | **通过** | 主站侧启动、单帧 NMT 广播复位、从机 boot-up 观察和安全退出均通过；。  |

## P0 验收结果

### P0-01：版本和依赖加载通过

目标板程序输出：

```text
canopen_slave_tester=0.3.0
stage=P1-runtime
built_against_lely=2.4.0
spdlog=1.17.0
```

该证据确认：

- 目标板可执行文件可以启动；
- 程序携带预期的软件阶段和依赖版本信息；
- Lely 动态库已能被目标板动态链接器加载，否则程序不会进入对象创建和 runtime 初始化。

结果：**PASS**。

### P0-02：运行配置加载通过

原始日志：

```text
2026-07-21 15:49:50.991 [info] [configuration] configuration validated
config=/opt/Ultra/Debug/canopen-slave-tester/config/tester.conf
can.interface=can1
can.bitrate=1000000
master.node_id=127
master.dcf_path=/opt/Ultra/Debug/canopen-slave-tester/config/master.dcf
slave.node_id=1
slave.eds_path=/opt/Ultra/Debug/canopen-slave-tester/config/project.eds
```

该证据确认部署后的 `tester.conf`、`master.dcf` 和 `project.eds` 路径有效，关键配置值被正确解析。

结果：**PASS**。

### P0-03：master.dcf 已加载通过

Lely 成功按 DCF 创建对象：

```text
creating object 1000
creating object 1001
creating object 1018
...
creating object 1F80
creating object 1F81
...
creating object 5E00
```

并进入：

```text
NMT: running as master
NMT: entering pre-operational state
creating LSS
creating Server-SDO 1
creating SYNC service
creating TIME service
creating EMCY producer service
```

该证据确认 `master.dcf` 不仅存在，而且已被 Lely 解析并用于创建主站对象字典和通信服务。

结果：**PASS**。

### P0-04：DCF 配置变更已部署生效

修改 `master.yml` 并重新生成、部署 DCF 后，主站启动行为由 Node-ID 2～126 的逐节点复位变为：

```text
NMT: sending command specifier 130 to node 0
```

该行为对应总线帧：

```text
000#8200
```

说明目标板加载的是更新后的 DCF，而不是旧配置。

结果：**PASS**。

### P0 证据归档缺口

本轮没有提供以下原始输出，因此本文不把它们列为已归档证据：

- 干净 CMake 配置和交叉编译完整日志；
- `build/canopen_slave_tester` 和 map 文件校验值；
- 本地与目标板部署文件 SHA-256 对照；
- CLI、配置、SocketCAN 和 runtime 各失败路径的退出码实测矩阵。

这些缺口影响证据完整性，但当前目标板实机运行已经证明 P0 的运行前置条件可用。

## P1 验收结果

### P1-01：Lely runtime 启动通过

原始日志：

```text
2026-07-21 15:49:51.050 [info] [runtime] Lely runtime is ready
2026-07-21 15:49:51.050 [info] [runtime] event loop started
```

结果：**PASS**。

### P1-02：NMT 启动流量优化通过

修改前主站向 Node-ID 2～126 连续发送：

```text
82 02
82 03
...
82 7E
```

从机统计显示：

```text
Total.receive.packages: 0000000126
Dropped.receive.packages: 0000000050
Receive error count: 0000000000
Ack/Bit/CRC/Format error count: 0000000000
Error code: No Error
```

这表明此前异常属于 MCU 软件接收队列丢包，而不是 CAN 物理层或控制器协议错误。

将 `mcu_node_1.reset_communication` 改为 `true` 并重新生成 DCF 后，主站只发送：

```text
NMT: sending command specifier 130 to node 0
```

修改后日志中没有再出现 Node-ID 2～126 的逐节点 Reset Communication。

结果：**PASS**。

### P1-03：从机接收广播复位通过

MCU 原始日志：

```text
RX ID:77f IDE:0 RTR:0 DLC:1
data: 00
RX ID:000 IDE:0 RTR:0 DLC:2
data: 82 00
communication reset requested: dev=can1
CAN module disabled
CANopen reset communication: dev=can1 node=1 bitrate=1000
CAN module initialized: dev=can1 bitrate=1000 kbit/s rx=17 tx=10
CAN RX thread started
CAN normal mode requested
CANopen running: dev=can1 node=1 bitrate=1000
TX ID:701 IDE:0 RTR:0 DLC:1
```

该证据确认：

- 从机收到主站 Node-ID 127 boot-up `77F#00`；
- 从机收到广播 Reset Communication `000#8200`；
- CANopenNode 通信对象被重新初始化；
- 从机发送 `701#00` boot-up。

结果：**PASS**。

### P1-04：主站观察从机 boot-up 通过

主站原始日志：

```text
NMT: booting slave 1
NMT: slave 1 finished booting with error status 0
NMT: all mandatory slaves started successfully
2026-07-21 15:49:51.056 [info] [canopen] boot node=1 state=0x0 status=success
2026-07-21 15:49:51.056 [info] [canopen] NMT state node=1 state=0x0
```

结果：**PASS**。

日志在约 10 秒后再次记录一次 boot success。对应测试期间 MCU 被人工执行 `reboot`，因此这是从机重新上线事件，不是主站重复发送广播复位。

### P1-05：运行期间未观察到 0x8110

修改后的主站日志没有出现：

```text
EMCY: received 8110 10
```

MCU 在广播复位后持续周期发送 `0x181` TPDO，未观察到通信复位死锁、BUS-OFF 或系统异常。

结果：**PASS**。

注意：P1 只确认运行时能够接收该 TPDO，不验证 PDO 映射值和应用语义。

### P1-06：安全退出通过

原始日志：

```text
2026-07-21 15:50:10.699 [info] [signal] SIGINT received
2026-07-21 15:50:10.699 [info] [runtime] stop requested
2026-07-21 15:50:10.699 [info] [runtime] event-loop shutdown started
2026-07-21 15:50:10.699 [info] [runtime] event loop stopped after 97 tasks
2026-07-21 15:50:10.742 [info] [runtime] runtime resources released
2026-07-21 15:50:10.742 [info] [runtime] session=1 lifecycle=stopped can_state=active can_errors=0 dropped_logs=0 last_event=NMT state node=1 state=0x0
```

该证据确认：

- `SIGINT` 被转换为正常停止请求；
- event loop 正常停止；
- Lely 服务和对象按顺序释放；
- 主机 CAN 状态保持 active；
- `can_errors=0`；
- `dropped_logs=0`。

结果：**PASS**。


## P1 最终判定

当前可确认：

```text
主站配置加载：PASS
Lely runtime 启动：PASS
单帧广播 Reset Communication：PASS
从机通信复位和 boot-up：PASS
主站 boot callback：PASS
运行期无 0x8110：PASS
安全退出：PASS
```