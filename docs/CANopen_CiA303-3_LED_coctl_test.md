# CANopen CiA 303-3 LED 人工测试指南（Lely `coctl`）

## 1. 目的

本文用于在Linux 主站上使用 Lely `coctl` 和少量 `can-utils` 命令，人工制造 CANopen 状态与通信故障，并直接观察 RT-Thread + CANopenNode MCU 从机的 CiA 303-3 RUN/ERROR LED。

本测试不修改主机自动测试程序，不要求主机软件自动判定 LED。测试人员按照本文顺序逐项执行，并以 MCU 实际 LED 视觉状态、`coctl` EMCY/NMT 输出及必要的 MCU 串口日志作为辅助证据。

当前测试环境：

- Linux 主站：
- CAN 接口：`can1`
- CAN 波特率：1 Mbit/s
- Lely 工具：`coctl`
- `coctl` 本地 CANopen Node-ID：127
- MCU 从机 Node-ID：1
- 目标目录：`/opt/Ultra/Debug/canopen-master`
- DCF：`/opt/Ultra/Debug/canopen-master/config/master.dcf`
- 从机默认 EMCY COB-ID：`0x081`
- 从机默认 Heartbeat COB-ID：`0x701`
- 从机默认 SYNC CAN-ID：`0x080`
- 从机 RPDO1 CAN-ID：`0x201`
- 从机 RPDO1 当前测试映射长度：4 bytes


---

## 2. 当前固件对 LED 测试最重要的行为

当前 `CO_app_RTT.c` 使用：

```c
#define CO_APP_RTT_NMT_CONTROL          (CO_NMT_STARTUP_TO_OPERATIONAL \
                                       | CO_NMT_ERR_ON_ERR_REG \
                                       | CO_ERR_REG_GENERIC_ERR \
                                       | CO_ERR_REG_COMMUNICATION)
```

因此，只要通信错误使 Error Register 的 Communication bit 置位，节点处于 Operational 时可能自动转换为 Pre-operational。

这会直接影响 RUN Green LED：

```text
Operational
    |
    | communication error
    v
Pre-operational
    |
    +--> RUN Green 从常亮变成 2.5 Hz 均匀闪烁
```

当前配置没有 `CO_NMT_ERR_FREE_TO_OPERATIONAL`，所以错误恢复后节点通常仍保持 Pre-operational，需要再次执行：

```text
start
```

才能回到 Operational。

这也是 Heartbeat、CAN Warning/Error Passive 等测试中“红灯变化时绿灯也开始闪”的原因，不应判为 LED 异常。

### 2.1 ERROR Red LED 显示优先级

当前 CANopenNode `CO_LEDs_process()` 的 ERROR LED 优先级从高到低为：

```text
CAN Bus-Off                     -> ON
CANopen Initializing/未配置     -> Flickering 10 Hz
RPDO timeout                    -> Quadruple flash
SYNC timeout                    -> Triple flash
Heartbeat Consumer error        -> Double flash
CAN Warning / Error Passive     -> Single flash
Other Error                     -> Blinking 2.5 Hz
无错误                          -> OFF
```

如果多个错误同时存在，只显示优先级更高的一项。

例如：

```text
Heartbeat Consumer error + CAN Error Passive
```

最终红灯显示：

```text
Double flash
```

而不是 Single flash。

### 2.2 RUN Green LED 显示优先级

当前 CANopenNode RUN LED 优先级从高到低为：

```text
LSS Configuration              -> Flickering 10 Hz
Firmware Download              -> Triple flash
NMT Stopped                    -> Single flash
NMT Pre-operational            -> Blinking 2.5 Hz
NMT Operational                -> ON
其他/Initializing              -> OFF
```

---

## 3. LED Pattern 肉眼识别表

现场测试不要只看 `Blinking`、`Triple flash` 等英文名称，按下面的视觉节奏判断。

| Pattern | 肉眼观察效果 |
|---|---|
| OFF | 始终熄灭 |
| ON | 始终常亮 |
| Flickering 10 Hz | 约亮 50 ms -> 灭 50 ms -> 持续重复，明显很快 |
| Blinking 2.5 Hz | 约亮 200 ms -> 灭 200 ms -> 持续重复 |
| Single flash | 闪 1 次（约 200 ms）-> 长灭约 1 s -> 重复 |
| Double flash | 连续闪 2 次 -> 长灭约 1 s -> 再闪 2 次 -> 重复 |
| Triple flash | 连续闪 3 次 -> 长灭约 1 s -> 再闪 3 次 -> 重复 |
| Quadruple flash | 连续闪 4 次 -> 长灭约 1 s -> 再闪 4 次 -> 重复 |

---

## 4. 将 `coctl` 部署

### 4.1 在交叉编译主机确认二进制

在 PC 的 `lely-core` 目录执行：

```sh
cd lely-core
file build-imx8p/tools/.libs/coctl
```

目标应为 AArch64 ELF，例如输出包含：

```text
ELF 64-bit LSB executable, ARM aarch64
```

可选检查动态依赖：

```sh
readelf -d build-imx8p/tools/.libs/coctl | grep NEEDED
```

### 4.2 SCP 到目标板

执行：

```sh
scp build-imx8p/tools/.libs/coctl \
    root@172.168.1.130:/opt/Ultra/Debug/canopen-master/coctl
```

登录目标板：

```sh
ssh root@172.168.1.130
```

目标板执行：

```sh
cd /opt/Ultra/Debug/canopen-master
chmod +x ./coctl
file ./coctl
```

当前项目的 Lely 动态库放在：

```text
/opt/Ultra/Debug/canopen-master/lib
```

所以先配置：

```sh
export LD_LIBRARY_PATH=/opt/Ultra/Debug/canopen-master/lib:${LD_LIBRARY_PATH:-}
```

检查：

```sh
ldd ./coctl
```

确认没有必要库显示 `not found`。

---

## 5. 测试前环境准备

### 5.1 停止现有 `canopen_master`

人工测试时不要同时运行项目自动 `canopen_master`，避免两个 CANopen master 同时控制 NMT、Heartbeat 和通信参数。

检查：

```sh
pgrep -a canopen_master
```

如果存在，优先使用 SIGINT：

```sh
pkill -INT -x canopen_master
sleep 1
pgrep -a canopen_master
```

### 5.2 检查 CAN

```sh
ip -details link show can1
```

正常测试前应为 UP，bitrate 为 `1000000`。

如果需要重新配置：

```sh
ip link set can1 down
ip link set can1 type can bitrate 1000000
ip link set can1 up
```

### 5.3 检查辅助工具

```sh
command -v candump
command -v cansend
```

本文中：

- NMT、SDO、Heartbeat、LSS：使用 `coctl`；
- 错误 SYNC、SYNC timeout、RPDO timeout：需要 `cansend`；
- CAN Warning/Error Passive：需要 Linux `ip link` 配合 MCU 自身持续发送；
- Bus-Off：当前普通 `coctl + can-utils` 环境不能稳定验收，见 TEST-11。

---

## 6. 启动 `coctl`

### 6.1 终端 A：运行 `coctl`

```sh
cd /opt/Ultra/Debug/canopen-master
export LD_LIBRARY_PATH=/opt/Ultra/Debug/canopen-master/lib:${LD_LIBRARY_PATH:-}
./coctl can1 ./config/master.dcf
```

进入 `coctl` 后，**每条命令单独输入一行，看到 `OK` 或返回值后再输入下一条**：

```text
set network 1
set id 127
init 0
set node 1
info version
r 0x1000 0 u32
r 0x1001 0 u8
```

不要一次粘贴多条命令后让终端把下一条拼到上一条末尾。实测已经出现过：

```text
error: expected data type
error: expected command
```

这类错误通常是输入行被粘连，不代表 CANopen 或 SDO 本身失败。

### 6.2 `init 0` 的实际作用

当前 `master.dcf` 在 `init 0` 后会执行 Node 1 boot/configuration，实测会对 Node 1 配置 Heartbeat 相关对象，包括：

```text
0x1016:01
0x1017:00
```

当前实测基线：

```text
0x1016:01 = 0x007F05DC
0x1017:00 = 500 ms
```

因此每次重新执行 `init 0` 后，应重新读取关键对象，不要假定前一个测试留下的临时值仍存在。

### 6.3 终端 B：抓包

```sh
cd /opt/Ultra/Debug/canopen-master
candump -tA -x can1
```

### 6.4 终端 C：执行 `cansend` 或网络接口命令

需要时执行：

```sh
cansend can1 ...
```

或：

```sh
ip link set can1 down
ip link set can1 up
```

---

# 7. LED 测试执行顺序

建议严格按照 TEST-01 -> TEST-12 的顺序执行。

每一个 ERROR LED 测试结束后，必须先恢复到：

```text
0x1001 = 0
ERROR Red = OFF
```

再执行下一项。

---

## TEST-01：NMT Operational

### 目的

验证 RUN Green 常亮。

### 操作

`coctl`：

```text
set node 1
start
```

等待约 1 秒。

### 预期 LED

| LED | 状态 | 肉眼观察 |
|---|---|---|
| RUN Green | **Operational / ON** | 绿灯持续常亮，不闪 |
| ERROR Red | **OFF** | 红灯持续熄灭 |

辅助确认：

```text
r 0x1001 0 u8
```

应为：

```text
0x00
```

### PASS

```text
RUN Green 常亮
ERROR Red 熄灭
```

---

## TEST-02：NMT Pre-operational

### 操作

```text
preop
```

### 预期 LED

| LED | 状态 | 肉眼观察 |
|---|---|---|
| RUN Green | **Pre-operational / Blinking 2.5 Hz** | 绿灯约亮 200 ms -> 灭 200 ms -> 持续均匀闪烁 |
| ERROR Red | **OFF** | 红灯持续熄灭 |

### PASS

绿灯应是持续、均匀的 2.5 Hz 闪烁，不是“闪一次后长灭”。

---

## TEST-03：NMT Stopped

### 操作

```text
stop
```

### 预期 LED

| LED | 状态 | 肉眼观察 |
|---|---|---|
| RUN Green | **Stopped / Single flash** | 绿灯闪 1 次 -> 长灭约 1 s -> 再闪 1 次 -> 重复 |
| ERROR Red | **OFF** | 红灯持续熄灭 |

### 恢复

```text
start
```

确认：

```text
RUN Green = 常亮
ERROR Red = 熄灭
```

---

## TEST-04：LSS Configuration State

### 目的

验证 RUN Green 10 Hz Flickering。

> `lss_switch_glob 1` 是广播操作。如果同一 CAN 总线上有多个 LSS slave，会同时进入 Configuration State。

### 操作

```text
lss_switch_glob 1
```

### 预期 LED

| LED | 状态 | 肉眼观察 |
|---|---|---|
| RUN Green | **LSS Configuration / Flickering 10 Hz** | 绿灯约亮 50 ms -> 灭 50 ms -> 快速重复；明显快于 Pre-op |
| ERROR Red | **OFF（无其他错误时）** | 红灯持续熄灭 |

实测 `coctl` 已能正常执行：

```text
lss_switch_glob 1
...
OK
```

### 恢复

```text
lss_switch_glob 0
start
```

确认绿灯恢复常亮。

---

## TEST-05：Other Error - Unexpected SYNC Data Length

### 目的

通过错误长度 SYNC 制造 `Other Error`，验证 ERROR Red 2.5 Hz Blinking。

当前实测：

```text
0x1005 = 0x00000080
0x1019 = 0
```

当 `0x1019=0` 时，标准 SYNC 数据长度应为 0。发送 DLC=1 的 SYNC 会触发 Unexpected SYNC data length。

### 5.1 前置

`coctl`：

```text
preop
r 0x1005 0 u32
r 0x1019 0 u8
r 0x1001 0 u8
```

确认类似：

```text
0x1005 = 0x00000080
0x1019 = 0x00
0x1001 = 0x00
```

### 5.2 制造错误

普通 shell：

```sh
cansend can1 080#00
```

这是 CAN-ID `0x080`、DLC=1 的错误 SYNC。

当前实测 `coctl` 收到：

```text
EMCY 8240 10
```

含义：

```text
0x8240 = Unexpected SYNC data length
0x10   = Communication Error Register bit
```

### 预期 LED

| LED | 状态 | 肉眼观察 |
|---|---|---|
| RUN Green | **Pre-operational / Blinking 2.5 Hz** | 绿灯约亮 200 ms -> 灭 200 ms -> 持续均匀闪烁 |
| ERROR Red | **Other Error / Blinking 2.5 Hz** | 红灯约亮 200 ms -> 灭 200 ms -> 持续均匀闪烁 |

这里 RUN 和 ERROR 都可能以 2.5 Hz 闪烁，但它们表示的含义不同：

- RUN：Pre-operational；
- ERROR：Other Error。

### 恢复

当前 `CO_EM_SYNC_LENGTH` 不通过普通合法 SYNC 自动清除，直接：

```text
reset comm
```

等待 Node 1 Boot 后：

```text
set node 1
start
r 0x1001 0 u8
```

确认：

```text
0x1001 = 0x00
RUN Green = 常亮
ERROR Red = 熄灭
```

---

## TEST-06：Heartbeat Consumer Error

### 目的

验证 ERROR Red Double flash，并验证当前 NMT 策略下 RUN Green 会从 Operational 自动变为 Pre-operational Blinking。

当前测试拓扑：

```text
coctl Node-ID 127
    |
    | Heartbeat 500 ms
    v
MCU Node 1 Heartbeat Consumer
    timeout = 1500 ms
```

当前实测：

```text
0x1016:01 = 0x007F05DC
```

其中：

```text
Producer Node-ID = 127 = 0x7F
Timeout          = 1500 ms = 0x05DC
```

### 6.1 建立正常 Heartbeat

`coctl`：

```text
set heartbeat 500
set node 1
r 0x1016 1 u32
```

如果不是 `0x007F05DC`，再写：

```text
w 0x1016 1 u32 0x007F05DC
```

然后：

```text
start
```

等待 2~3 秒。

正常预期：

```text
RUN Green = 常亮
ERROR Red = 熄灭
```

### 6.2 停止 `coctl` Heartbeat

```text
set heartbeat 0
```

等待超过 1500 ms。

当前实测收到：

```text
EMCY 8130 10
ERRORx PREO
```

并且：

```text
r 0x1001 0 u8
```

实测为：

```text
0x10
```

状态链：

```text
Heartbeat Consumer timeout
    |
    +--> EMCY 0x8130
    +--> Error Register communication bit = 0x10
    +--> ERROR Red = Double flash
    |
    +--> CO_NMT_ERR_ON_ERR_REG
            |
            v
        Operational -> Pre-operational
            |
            +--> RUN Green = Blinking 2.5 Hz
```

### 预期 LED

| LED | 状态 | 肉眼观察 |
|---|---|---|
| RUN Green | **Pre-operational / Blinking 2.5 Hz** | 绿灯约亮 200 ms -> 灭 200 ms -> 持续均匀闪烁 |
| ERROR Red | **Heartbeat Consumer / Double flash** | 红灯连续闪 2 次 -> 长灭约 1 s -> 再闪 2 次 -> 重复 |

**绿灯同时闪烁是当前固件的正常预期，不是异常。**

### 6.3 恢复 Heartbeat

```text
set heartbeat 500
```

当前实测会收到：

```text
EMCY 0000 00
```

此时预期：

| LED | 状态 | 肉眼观察 |
|---|---|---|
| RUN Green | **仍为 Pre-operational / Blinking 2.5 Hz** | 绿灯继续约 200 ms 亮 / 200 ms 灭 |
| ERROR Red | **OFF** | 红灯熄灭 |

原因是当前没有 `CO_NMT_ERR_FREE_TO_OPERATIONAL`。

最后执行：

```text
start
```

最终：

```text
RUN Green = 常亮
ERROR Red = 熄灭
```

---

## TEST-07：SYNC Reception Timeout

### 目的

验证 ERROR Red Triple flash。

当前 CANopenNode 的 SYNC timeout 条件为：

```text
timeout ~= 1.5 * 0x1006 communication cycle period
```

并且必须先收到至少一次合法 SYNC，才开始后续 timeout 监督。

### 7.1 设置 SYNC 周期

`coctl`：

```text
preop
r 0x1006 0 u32
```

记录原值：

```text
<ORIG_1006>
```

当前实测原值：

```text
0x00000000
```

设置 500 ms：

```text
w 0x1006 0 u32 500000
```

单位为 us：

```text
500000 us = 500 ms
```

### 7.2 发送一次合法 SYNC，然后停止

普通 shell：

```sh
cansend can1 080#
```

之后不要继续发送 SYNC。

等待：

```text
> 750 ms
```

建议实际观察 1~2 秒。

当前实测已经观察到 SYNC timeout EMCY：

```text
EMCY 8100 10
```

其中：

```text
0x8100 = Communication error（此处由 CO_EM_SYNC_TIME_OUT 报告）
0x10   = Communication Error Register bit
```

### 预期 LED

| LED | 状态 | 肉眼观察 |
|---|---|---|
| RUN Green | **Pre-operational / Blinking 2.5 Hz** | 绿灯约亮 200 ms -> 灭 200 ms -> 持续均匀闪烁 |
| ERROR Red | **SYNC Timeout / Triple flash** | 红灯连续闪 3 次 -> 长灭约 1 s -> 再闪 3 次 -> 重复 |

由于本测试从 Pre-operational 开始，通信错误不会再引起一次额外的 NMT 状态变化，适合稳定观察 Triple flash。

### 7.3 恢复

先发送一个合法 SYNC：

```sh
cansend can1 080#
```

SYNC timeout 清除后应出现 EMCY reset：

```text
EMCY 0000 00
```

再恢复 `0x1006`：

```text
w 0x1006 0 u32 <ORIG_1006>
```

当前原值为 0 时：

```text
w 0x1006 0 u32 0
```

确认：

```text
r 0x1001 0 u8
```

应恢复 `0x00`。

最后：

```text
start
```

---

## TEST-08：RPDO Event Timer Timeout

### 目的

验证 RPDO timeout 条件及 CiA 303-3 Quadruple flash 状态。

### 重要结论

**当前固件能够触发 RPDO timeout，但由于 NMT error policy 会立即让节点从 Operational 切到 Pre-operational，RPDO timeout 随后被清除，所以 ERROR Red Quadruple flash 不能稳定保持到足够肉眼观察的时间。**

因此本项分为：

- RPDO timeout 协议触发：可验证；
- Quadruple flash 肉眼稳定验收：当前配置记为 `LIMITED/SKIP`，不要因为没看清四闪就判 LED 实现失败。

### 8.1 设置 Event Timer

`coctl`：

```text
set node 1
start
r 0x1400 5 u16
```

记录原值：

```text
<ORIG_1400_05>
```

当前实测原值：

```text
0x0000
```

设置 1000 ms：

```text
w 0x1400 5 u16 1000
```

### 8.2 明确发送一次有效 RPDO1

当前项目 RPDO1：

```text
COB-ID = 0x201
DLC    = 4
```

普通 shell：

```sh
cansend can1 201#00000000
```

收到这帧后 RPDO timeout 监督开始。

之后不要再发送 RPDO1，等待超过：

```text
1000 ms
```

### 8.3 当前实测行为

实测 `coctl`：

```text
EMCY 8250 10
EMCY 0000 00
ERRORx PREO
```

状态链：

```text
Operational
    |
    | RPDO 超过 event timer 未收到
    v
RPDO timeout
    |
    +--> EMCY 0x8250
    +--> ERROR Red 进入 Quadruple flash 条件
    +--> Error Register communication bit = 0x10
              |
              v
        Operational -> Pre-operational
              |
              v
        RPDO timeout 监督不再保持
              |
              +--> EMCY 0x0000
              +--> RPDO timeout error 清除
```

因此正常肉眼可能看到：

| LED | 实际预期 |
|---|---|
| RUN Green | 很快变为 **Pre-operational Blinking：约亮 200 ms -> 灭 200 ms -> 重复** |
| ERROR Red | Quadruple flash 条件只短暂存在，**不保证能稳定看到完整“闪 4 次 -> 长灭”周期** |

### PASS 判据

协议侧：

```text
看到 EMCY 0x8250
```

即可证明 RPDO Event Timer Timeout 已触发。

如果紧接着出现：

```text
EMCY 0x0000
ERRORx PREO
```

在当前 NMT 配置下属于预期行为。

### 8.4 恢复

恢复 Event Timer：

```text
w 0x1400 5 u16 <ORIG_1400_05>
```

当前原值为 0：

```text
w 0x1400 5 u16 0
```

然后：

```text
start
r 0x1001 0 u8
```

最终确认：

```text
RUN Green = 常亮
ERROR Red = 熄灭
0x1001 = 0x00
```

---

## TEST-09：Node-ID Unconfigured / CANopen Initializing

### 目的

稳定制造 CANopen Node-ID 未配置状态，验证：

```text
RUN Green  = OFF
ERROR Red  = Flickering 10 Hz
```

此项会暂时让 Node 1 的普通 SDO/NMT/Heartbeat 失效，因此放在后半段执行。

**本测试不要执行 `lss_store`，避免永久写入 Node-ID。**

### 9.1 设置临时 Node-ID 255

`coctl`：

```text
preop
lss_switch_glob 1
lss_set_node 255
lss_switch_glob 0
```

此时 active Node-ID 仍可能暂时是 1。

执行：

```text
set node 1
reset comm
```

Reset Communication 后节点使用 pending Node-ID `255/0xFF`，进入 unconfigured 状态。

### 预期 LED

| LED | 状态 | 肉眼观察 |
|---|---|---|
| RUN Green | **OFF** | 绿灯持续熄灭 |
| ERROR Red | **Initializing / Flickering 10 Hz** | 红灯约亮 50 ms -> 灭 50 ms -> 快速重复 |

同时预期：

- Node 1 普通 SDO 暂时不可用；
- Node 1 Heartbeat 不再正常持续；
- LSS 仍可使用。

实测已经执行过 `lss_set_node 255` 和 `reset comm`，`coctl` 随后检测 Node 1 Heartbeat lost，符合普通 Node 1 已暂时不可达的现象。

### 9.2 恢复 Node-ID=1

```text
lss_switch_glob 1
lss_set_node 1
lss_switch_glob 0
```

当前实测中，退出 LSS Configuration 后会重新开始 Node 1 boot/configuration。

等待完成后：

```text
set node 1
start
r 0x1001 0 u8
```

最终确认：

```text
RUN Green = 常亮
ERROR Red = 熄灭
0x1001 = 0x00
```

---

## TEST-10：CAN Warning / Error Passive LED

### 目的

验证 CiA 303-3 ERROR Red Single flash。

### 10.1 当前 RT-Thread port 的 CAN 错误阈值

当前 `CO_driver_rtthread.c` 使用 RT-Thread：

```text
status.snderrcnt -> 日志中的 tx_err
status.rcverrcnt -> 日志中的 rx_err
```

发送错误判定：

```text
tx_err >= 256  -> TX Bus-Off
tx_err >= 128  -> TX Error Passive
tx_err >= 96   -> TX Warning
```

接收错误判定：

```text
rx_err >= 128  -> RX Error Passive
rx_err >= 96   -> RX Warning
```

CANopenNode LED 使用 `CO_CAN_ERR_WARN_PASSIVE`，因此：

```text
TX/RX Warning
TX/RX Error Passive
```

都会进入同一个：

```text
ERROR Red = Single flash
```

### 10.2 为什么不要求必须抓到 `96 <= tx_err < 128`

普通“无人 ACK”会让发送错误快速增长，软件采样时可能直接看到：

```text
tx_err = 128
```

而没有稳定记录到 96~127。

当前 MCU 实测已经记录：

```text
CAN status changed: rx_err=0 tx_err=128 ... err=0x0002
CAN status changed: rx_err=0 tx_err=128 ... err=0x000a
```

其中当前 port 中：

```text
0x0002 = TX Error Passive
0x000a = TX Error Passive + TX Overflow
```

这已经进入 CANopenNode 的 Warning/Passive LED 条件，因此可用于验证 Single flash。

### 10.3 先关闭从机 Heartbeat Consumer，避免 Double flash 抢占

这是本测试的关键步骤。

如果保留：

```text
0x1016:01 = 0x007F05DC
```

随后执行 `ip link set can1 down`，Node 127 Heartbeat 也会消失，从机会产生 Heartbeat Consumer error。由于 Double flash 优先级高于 Single flash，最终红灯会显示 Double flash，无法观察 CAN Warning/Passive 的 Single flash。

所以先：

```text
set node 1
r 0x1016 1 u32
```

记录：

```text
<ORIG_1016_01>
```

当前通常为：

```text
0x007F05DC
```

临时关闭：

```text
w 0x1016 1 u32 0
r 0x1016 1 u32
```

必须读回：

```text
0x00000000
```

当前实测已成功执行该写入和 readback。

### 10.4 确保 MCU 持续发送 CAN 帧

记录从机 Producer Heartbeat：

```text
r 0x1017 0 u16
```

当前通常：

```text
500 ms
```

为了更快产生发送错误，临时设置：

```text
w 0x1017 0 u16 100
start
```

确认初始：

```text
RUN Green = 常亮
ERROR Red = 熄灭
```

### 10.5 制造无人 ACK 环境

在普通 Linux shell 执行：

```sh
ip link set can1 down
```

这会让 TQ8MP CAN controller 退出总线，MCU 继续发送 Heartbeat/EMCY 时没有其他节点 ACK。

MCU 串口应出现发送失败及 CAN error counter 变化，例如当前实测：

```text
canopen.rtt: tx failed: id=0x081 written=0 ret=-15
canopen.rtt: CAN status changed: rx_err=0 tx_err=128 ... err=0x0002
```

或者伴随 TX overflow：

```text
err=0x000a
```

### 预期 LED

| LED | 状态 | 肉眼观察 |
|---|---|---|
| RUN Green | **Pre-operational / Blinking 2.5 Hz** | 绿灯约亮 200 ms -> 灭 200 ms -> 持续均匀闪烁 |
| ERROR Red | **CAN Warning/Error Passive / Single flash** | 红灯闪 1 次 -> 长灭约 1 s -> 再闪 1 次 -> 重复 |

绿灯变为 Pre-operational Blinking 是正常的：CAN communication error 使 Error Register 置位，当前 `CO_NMT_ERR_ON_ERR_REG` 会让节点从 Operational 退到 Pre-operational。

### PASS 判据

同时满足：

```text
Heartbeat Consumer 已临时关闭：0x1016:01 = 0
MCU 日志进入 Warning/Passive 条件，当前实测可见 tx_err=128
ERROR Red = Single flash
RUN Green = Pre-op 2.5 Hz Blinking
```

不要求必须稳定捕获 `tx_err=96..127`。

如果红灯是：

```text
连续闪 2 次 -> 长灭
```

说明 Heartbeat Consumer error 仍然存在或没有正确关闭，应先排除 Heartbeat error 再重新测试。

### 10.6 恢复

先恢复 Linux CAN：

```sh
ip link set can1 up
ip -details link show can1
```

由于 `coctl` 在 `can1 down` 期间会失去通信，建议直接退出并重新初始化：

```text
Ctrl+C
```

Shell：

```sh
cd /opt/Ultra/Debug/canopen-master
export LD_LIBRARY_PATH=/opt/Ultra/Debug/canopen-master/lib:${LD_LIBRARY_PATH:-}
./coctl can1 ./config/master.dcf
```

重新：

```text
set network 1
set id 127
init 0
set node 1
r 0x1016 1 u32
r 0x1017 0 u16
r 0x1001 0 u8
```

当前 DCF boot/configuration 通常会重新写回：

```text
0x1016:01 = 0x007F05DC
0x1017:00 = 500
```

仍应以 readback 为准。如果没有恢复，写回测试前记录值：

```text
w 0x1016 1 u32 <ORIG_1016_01>
w 0x1017 0 u16 500
```

最后：

```text
start
```

确认：

```text
RUN Green = 常亮
ERROR Red = 熄灭
0x1001 = 0x00
```

---

## TEST-11：CAN Bus-Off

### 目的

CiA 303-3 Bus-Off 对应：

```text
ERROR Red = 常亮 ON
```

### 当前项目结论

**当前普通 `coctl + ip link set can1 down` 方法不能作为 Bus-Off PASS 方案。**

2026-08-12 实测无人 ACK 时，MCU 串口持续看到：

```text
tx_err=128
err=0x0002 / 0x000a
```

即 TX Error Passive（以及可能的 TX overflow），没有拿到 Bus-Off 状态证据。

因此 TEST-10 可以利用这个环境验证 Single flash，但 TEST-11 不应把同一操作误判为 Bus-Off。

### 当前执行状态

如果现场只有：

```text
coctl
cansend/candump
ip link
```

本项记录：

```text
SKIP - 当前工具无法稳定制造并确认 MCU CAN Bus-Off
```

### 如果后续具备 CAN Fault Injection 工具

需要使用支持主动 CAN 错误注入的设备，使 MCU CAN controller 真正进入 Bus-Off，并同时确认 RT-Thread/CANopen port 报告 Bus-Off 状态。

达到真实 Bus-Off 后，当前 LED 优先级预期：

| LED | 状态 | 肉眼观察 |
|---|---|---|
| ERROR Red | **Bus-Off / ON** | 红灯持续常亮，不闪 |
| RUN Green | 通常由于通信错误进入 **Pre-operational Blinking** | 绿灯约亮 200 ms -> 灭 200 ms -> 重复 |

Bus-Off 是 ERROR Red 最高优先级，其他 Heartbeat/SYNC/RPDO/Warning 错误不能覆盖红灯常亮。

---

## TEST-12：Firmware Download In Progress

### 目的

CiA 303-3 Firmware Download 对应：

```text
RUN Green = Triple flash
```

### 当前工程结论

当前 CANopenNode `CANopen.c` 中，如果应用没有另外定义：

```c
CO_STATUS_FIRMWARE_DOWNLOAD_IN_PROGRESS
```

默认值为：

```c
false
```

当前 RT-Thread port 没有把 Qboot/FOTA 下载状态接到该宏，因此普通 `coctl` SDO 写入不会让 RUN LED 进入 Firmware Download Triple flash。

当前本项记录：

```text
N/A - 当前固件没有 Firmware Download LED 状态源
```

不要为了得到 Triple flash 而随意写一个 OD 对象并把它当作 PASS。

如果未来 Bootloader/FOTA 把真实下载状态接入 `CO_STATUS_FIRMWARE_DOWNLOAD_IN_PROGRESS`，再增加真正的固件下载流程验证。

---

# 8. 全状态验收表

## 8.1 RUN Green

| 场景 | 预期 | 当前测试方式 | 当前可验收性 |
|---|---|---|---|
| NMT Operational | ON | `start` | PASS |
| NMT Pre-operational | Blinking 2.5 Hz | `preop` | PASS |
| NMT Stopped | Single flash | `stop` | PASS |
| LSS Configuration | Flickering 10 Hz | `lss_switch_glob 1` | PASS |
| Firmware Download | Triple flash | 当前无状态源 | N/A |
| CANopen Initializing / Node-ID unconfigured | OFF | LSS 设置 Node-ID 255 + Reset Comm | PASS |

## 8.2 ERROR Red

| 场景 | 预期 | 当前测试方式 | 当前可验收性 |
|---|---|---|---|
| 无错误 | OFF | 正常 Operational | PASS |
| CANopen Initializing / Node-ID unconfigured | Flickering 10 Hz | LSS Node-ID 255 + Reset Comm | PASS |
| Other Error | Blinking 2.5 Hz | 错误长度 SYNC，EMCY `0x8240` | PASS |
| CAN Warning / Error Passive | Single flash | 关闭从机 HB consumer + `can1 down`，当前实测 `tx_err=128` | PASS（验证 Warning/Passive 共用 LED 路径） |
| Heartbeat Consumer Error | Double flash | `set heartbeat 0`，EMCY `0x8130` | PASS |
| SYNC Timeout | Triple flash | `0x1006=500000` + 发送一次 SYNC 后停止 | PASS |
| RPDO Timeout | Quadruple flash | `0x1400:05=1000` + 停止 RPDO | LIMITED：`0x8250` 可触发，但当前 NMT 策略会立即清除 |
| CAN Bus-Off | ON | 当前工具不足 | SKIP |

---

# 9. 推荐现场执行清单

按以下顺序执行，尽量减少状态互相污染：

```text
[ ] 环境：停止 canopen_master
[ ] 环境：can1 UP / 1 Mbit/s
[ ] 环境：启动 coctl，逐条执行 set network / set id / init / set node

[ ] TEST-01 Operational
    RUN 绿灯常亮；ERROR 红灯灭

[ ] TEST-02 Pre-operational
    RUN 绿灯约 200 ms 亮 / 200 ms 灭，持续均匀闪

[ ] TEST-03 Stopped
    RUN 绿灯闪 1 次 -> 长灭 -> 重复

[ ] TEST-04 LSS Configuration
    RUN 绿灯 10 Hz 快闪，约 50 ms 亮 / 50 ms 灭

[ ] TEST-05 Unexpected SYNC Data Length
    coctl 看到 EMCY 0x8240
    RUN 绿灯 Pre-op 均匀闪
    ERROR 红灯 2.5 Hz 均匀闪
    完成后 reset comm

[ ] TEST-06 Heartbeat Consumer
    coctl 看到 EMCY 0x8130
    RUN 绿灯 Pre-op 均匀闪
    ERROR 红灯 连闪 2 次 -> 长灭 -> 重复
    恢复 Heartbeat 后 ERROR 灭，RUN 仍闪；再 start 后 RUN 常亮

[ ] TEST-07 SYNC Timeout
    coctl 看到 EMCY 0x8100
    RUN 绿灯 Pre-op 均匀闪
    ERROR 红灯 连闪 3 次 -> 长灭 -> 重复

[ ] TEST-08 RPDO Timeout
    coctl 看到 EMCY 0x8250
    随后允许出现 EMCY 0x0000 + PREO
    Quadruple flash 当前不作为稳定肉眼 PASS 条件

[ ] TEST-09 Node-ID Unconfigured
    RUN 绿灯灭
    ERROR 红灯 10 Hz 快闪
    使用 LSS 恢复 Node-ID=1

[ ] TEST-10 CAN Warning / Error Passive
    先把从机 0x1016:01 临时写 0
    从机 0x1017 临时设 100 ms
    ip link set can1 down
    MCU 串口看到 tx_err=128 / err=0x0002 或 0x000a
    RUN 绿灯 Pre-op 均匀闪
    ERROR 红灯 闪 1 次 -> 长灭 -> 重复
    恢复 can1 后重新启动 coctl/init

[ ] TEST-11 Bus-Off
    当前 SKIP；不要把 tx_err=128 当 Bus-Off

[ ] TEST-12 Firmware Download
    当前 N/A

[ ] 最终恢复
    Node-ID = 1
    0x1016:01 恢复
    0x1017 恢复 500 ms
    0x1006 恢复
    0x1400:05 恢复
    0x1001 = 0
    RUN Green 常亮
    ERROR Red 熄灭
```

---

# 10. 测试结束后的统一恢复

如果 TEST-10 曾执行过 `can1 down`：

```sh
ip link set can1 up
ip -details link show can1
```

重新运行：

```sh
cd /opt/Ultra/Debug/canopen-master
export LD_LIBRARY_PATH=/opt/Ultra/Debug/canopen-master/lib:${LD_LIBRARY_PATH:-}
./coctl can1 ./config/master.dcf
```

逐条执行：

```text
set network 1
set id 127
init 0
set node 1
set heartbeat 500
r 0x1016 1 u32
r 0x1017 0 u16
r 0x1006 0 u32
r 0x1400 5 u16
r 0x1001 0 u8
start
```

当前基线通常应恢复为：

```text
0x1016:01 = 0x007F05DC
0x1017:00 = 500
0x1006:00 = 0
0x1400:05 = 0
0x1001:00 = 0
```

最终 LED：

```text
RUN Green = 常亮
ERROR Red = 熄灭
```

退出 `coctl` 后，再按原工程方式启动 `canopen_master`。

---


# 11. 关键注意事项

1. `coctl` 命令逐条执行，不要把多条命令粘成同一输入行。
2. `coctl` 与项目 `canopen_master` 不要同时作为 CANopen master 运行。
3. 当前通信错误会触发 `Operational -> Pre-operational`，所以很多 ERROR 测试中 RUN 绿灯同时开始 2.5 Hz 闪烁是正常现象。
4. ERROR Red 有显示优先级；Heartbeat Double flash 会覆盖 CAN Warning/Error Passive Single flash。
5. TEST-10 必须先把从机 `0x1016:01` 临时写 0，才能用无人 ACK 环境清晰观察 Single flash。
6. 当前 `tx_err=128` 就是 RT-Thread `status.snderrcnt=128`，表示发送侧已进入 Error Passive 条件，不是 Bus-Off。
7. TEST-08 的 `0x8250 -> 0x0000 -> PREO` 是当前 NMT error policy 导致的正常状态链，因此 Quadruple flash 不作为当前固件的稳定肉眼验收项。
8. TEST-11 当前没有可靠的普通工具 Bus-Off 方案；没有 Bus-Off 状态证据时必须记录 SKIP。
9. TEST-12 当前没有 Firmware Download LED 状态源，记录 N/A。
10. TEST-09 不执行 `lss_store`，避免 LED 测试永久修改 Node-ID。
11. 如果 LED 电平全部相反，优先检查实际硬件 LED 极性和 `PKG_CANOPENNODE_LEDS_RTT_PIN_*_ACTIVE_HIGH` 配置。
12. 每个故障测试结束后先确认 `0x1001=0`、ERROR Red=OFF，再执行下一项。
