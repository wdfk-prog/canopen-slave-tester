# A01-A03 自动测试设计

## 设计边界

当前自动流程直接复用单个 `lely::canopen::AsyncMaster`，不创建 `BasicDriver`、`SlaveSession` 或重复协议 Service。`main.cpp` 只负责 Lely/SocketCAN 生命周期、Startup Boot、流程注册和退出；A01/A02/A03 各自持有测试私有参数、等待状态和断言逻辑。

当前顺序：

```text
Startup Boot
→ A01 Heartbeat
→ A02 SDO
→ A03 PDO
→ 等待 Ctrl+C/SIGTERM
→ Final Reset Communication
```

注册流程采用 fail-fast：当前流程返回非零时，不执行后续自动流程。

## 文件职责

```text
src/main.cpp
├─ 创建并持有全部 Lely 和 SocketCAN 对象
├─ 注册 CAN/Boot/Heartbeat/EMCY callback
├─ 启动 Loop::run() 工作线程
├─ Reset 并等待 Startup Boot
├─ canopenRunProcesses(A01, A02, A03)
├─ 等待退出信号
├─ finalResetProcess()
└─ shutdown Context 并 join 线程

src/nmt_heartbeat.cpp
├─ Boot/Heartbeat/EMCY callback 和等待状态
├─ A01 私有 Heartbeat 参数
├─ 主站 Producer Heartbeat 中断/恢复
└─ 通过 SDO 中断/恢复从机 Producer Heartbeat

src/sdo_process.cpp
├─ A02 私有测试对象和 probe value
├─ SubmitRead/SubmitWrite completion 等待
└─ 0x2200:00 写入、回读、恢复和恢复确认

src/pdo_process.cpp
├─ A03 私有 PDO/OD/时序参数
├─ OnRpdo/OnTpdo callback 状态
├─ TPDO1 payload/周期/映射/OD 一致性检查
├─ Lely TpdoMapped() RPDO1 发送
└─ 0x2200:00 保存、回读和恢复

src/shutdown_process.cpp
└─ Reset Communication 并等待 Boot callback
```

## 流程私有配置

公共 `include/canopen_config.h` 只保留跨流程共享的运行环境配置和流程开关。阶段内部测试参数放在对应 `.cpp` 的匿名命名空间。

A01 当前私有参数包括：

```text
0x1017 Producer Heartbeat object
Heartbeat wait timeout = 3000 ms
Heartbeat activation sample count = 5
Producer Heartbeat period = 500 ms
```

A02 当前私有参数包括：

```text
0x2200:00 test object
primary probe = 0x12345678
alternate probe = 0x87654321
```

A03 当前私有参数包括：

```text
TPDO1 remote objects = 0x2100:00 + 0x2101:00
RPDO1 remote object = 0x2200:00
TPDO samples = 5
TPDO expected period = 1000 ms
TPDO tolerance = +/-150 ms
RPDO probe = 0xA5A5A5A5 / alternate 0x5A5A5A5A
```

`master.yml`、EDS 和 DCF 继续负责真正的 CANopen 通信参数；源码私有常量只描述测试策略，不替代 DCF 配置。

## Startup Boot

启动顺序保持：

```text
注册 callback
→ 启动 event loop
→ prepareBootWait()
→ master.Reset()
→ waitForBootCompletion()
```

Boot callback `status == 0` 或 `status == 'L'` 视为可继续自动流程。其他结果或等待超时阻止 A01-A03。

## A01 Heartbeat

A01 首先等待 5 个 500 ms Producer Heartbeat 周期，使双向 Heartbeat consumer 进入稳定监督状态，然后验证：

```text
prepare 0x8130 wait
→ local master 0x1017:00 = 0
→ wait slave EMCY 0x8130
→ prepare 0x0000 wait
→ local master 0x1017:00 = 500
→ wait slave EMCY reset
→ NMT Start node 1
```

随后验证从机 Producer Heartbeat：

```text
local 0x1F81:01 = 0
→ SubmitWrite(node 1, 0x1017:00, 0)
→ wait OnHeartbeat(node=1, true)
→ SubmitWrite(node 1, 0x1017:00, 500)
→ wait OnHeartbeat(node=1, false)
→ local 0x1F81:01 = 0x00000005
```

测试窗口临时清除本地主站的 slave assignment，避免 synthetic Heartbeat timeout 触发 Lely 默认 NMT reset。测试结束后恢复 assignment。

## A02 SDO

A02 使用 `SubmitRead<uint32_t>()` / `SubmitWrite()` 验证节点 1 的 `0x2200:00`：

```text
read original
→ choose probe != original
→ write probe
→ read-back == probe
→ restore original
→ read-back == original
```

协议级 SDO timeout 与本地 completion wait timeout 分开处理。本地 completion wait timeout 时远端事务状态被标记为未知，不继续提交可能冲突的恢复事务。

## A03 RPDO/TPDO

### PDO 映射基线

当前 EDS/DCF 已提供：

```text
Slave TPDO1 0x181, DLC 8
  0x2100:00 UNSIGNED32
  0x2101:00 UNSIGNED32
  event timer 1000 ms

Slave RPDO1 0x201, DLC 4
  0x2200:00 UNSIGNED32
```

主站 DCF 具有对应远端映射，因此 A03 不修改 EDS/DCF。

### NMT 状态

生成的主站 DCF 配置为 `start:false`，主站自身默认保持 Pre-operational。Lely PDO 服务要求本地节点处于 Operational，因此 A03 执行：

```text
NMT Start local master Node-ID 127
→ NMT Start slave Node-ID 1
→ PDO verification
→ keep local master Operational for later automatic stages
```

当 `BasicMaster::Command()` 的目标 Node-ID 等于主站自身 Node-ID 时，Lely 在本地执行 NMT 状态切换，不发送针对 Node 127 的网络 NMT 命令。

### TPDO1 验证

A03 在进入 Operational 前注册 `OnRpdo()`。从主站视角，从机 TPDO1 是主站 RPDO1。

`OnRpdo()` callback 只复制 PDO number、错误、payload 和 `steady_clock` 时间戳并唤醒等待线程，不执行 SDO 或长时间断言。

收集 5 个 TPDO1 后检查：

- callback 无处理错误；
- DLC 等于 8；
- payload `[0..3]` 按 little-endian 解码为 `0x2100:00`；
- payload `[4..7]` 按 little-endian 解码为 `0x2101:00`；
- 4 个连续周期均在 850~1150 ms。

随后比较：

```text
raw TPDO payload
== master.RpdoMapped(node 1) mapped value
== SDO read 0x2100:00 / 0x2101:00
```

若比较期间收到新 TPDO，则放弃当前快照并使用最新 generation 重试，避免跨周期比较旧 payload 和新 OD 值。

### RPDO1 验证

A03 先通过 SDO 保存 `0x2200:00`，然后选择一个与原值不同的 probe：

```text
TpdoMapped(node 1)[0x2200][0].Write(probe)
→ WriteEvent()
→ wait OnTpdo(PDO1)
→ validate DLC=4 and little-endian payload
→ SDO read 0x2200:00 == probe
```

主站 `TpdoMapped()`/`OnTpdo()` 对应从机 RPDO1，因为 PDO 命名始终以本地主站方向为准。

正常清理使用 SDO 恢复原 `0x2200:00` 并回读确认。若本地 SDO completion wait 状态未知，则不继续叠加 SDO 事务，改用 RPDO 做一次 best-effort 原值恢复，但该路径无法完成 SDO read-back，因此 A03 仍返回失败。

A03 结束时只注销 `OnRpdo()`/`OnTpdo()` callback；本地主站保持 Operational，供后续 PDO/SYNC 等自动阶段继续使用。实机验证表明将本地主站切回 Pre-operational 会触发 Lely master boot 管理并广播 Reset Communication，因此阶段清理不得执行该状态回退。

## 退出

收到 `SIGINT` 或 `SIGTERM` 后：

```text
finalResetProcess(master)
→ Reset Communication node 1
→ wait Boot callback
→ Context::shutdown()
→ join event-loop thread
```

由于 `boot:true`，Final Reset 会再次执行节点配置。主站随后退出导致节点 1 检测到主站 Heartbeat 丢失，属于真实离线行为，不计入 A01-A03 自动测试结果。

## 兼容性和影响范围

- 保持单个 `AsyncMaster` 和现有 event-loop 线程模型；
- 不引入 `BasicDriver`、第二套 CANopen 协议层或外部 `cansend`；
- 不修改 CANopenNode、RT-Thread、Lely、EDS、DCF 或 MCU 固件；
- 不改变 CAN 接口、Node-ID、目标部署目录或 CMake target；
- A01 的等待参数改为流程私有常量，其中 callback/SDO wait 为 3000 ms，稳定等待为 5 个 500 ms 周期；
- A02 测试对象和 probe value 改为流程私有命名；
- 新增 A03 流程开关和 `pdoProcess()` 工程内部接口。
