# CANopen Host 自动协议测试

本工程基于 Lely CANopen，面向 TQ8MP Linux/aarch64 与 RT-Thread + CANopenNode MCU。Host 角色直接由 `include/canopen_config.h` 中的 `CANOPEN_ROLE` 宏选择；当前默认 `CANOPEN_ROLE_MASTER`。Master process table 按固定顺序注册各 stage，但只有对应 `CANOPEN_ENABLE_*_PROCESS` 为 `1` 的流程才实际执行；当前默认启用 A06 EMCY、B06 EMCY Consumer 和 J03/B09G GFC。Slave role 使用 Lely `BasicSlave` Node 2 验证 MCU NMT Master。两种角色仍共用同一 `canopen_master` target 和全部 `src/*.cpp`。角色选择与 NMT 测试流程见 [`docs/CANopen_NMT_Master_Test.md`](docs/CANopen_NMT_Master_Test.md)。

## 自动测试链路

启动后，Lely 根据 `config/generated/master.dcf` 执行节点 1 的 Boot，并自动下载 `mcu_node_1.bin` 中的 Heartbeat 配置：

- 从机 `0x1016:01 = 0x007F05DC`：监控主站 Node-ID 127，超时 1500 ms；
- 从机 `0x1017:00 = 500`：每 500 ms 发送 Producer Heartbeat；
- 主站 `0x1016:01 = 0x000105DC`：监控从机 Node-ID 1，超时 1500 ms；
- 主站 `0x1017:00 = 500`：每 500 ms 发送 Producer Heartbeat。

Master stage 的固定顺序如下；当前实际执行项由各流程头文件中的 enable 宏决定：

```text
Startup Boot
→ A01 Heartbeat
   → 主站 Producer Heartbeat 中断/恢复
   → 从机 Producer Heartbeat SDO 中断/恢复
→ A02 SDO
   → 保存 0x2200:00
   → 写 probe value 并回读
   → 恢复原值并回读
→ A03 PDO
   → 本地主站和从机进入 Operational
   → 收集 TPDO1(0x181) 并检查 DLC/映射/周期
   → 比较 TPDO payload、Lely 映射值与 0x2100/0x2101 SDO 值
   → 通过 Lely PDO API 发送 RPDO1(0x201)
   → SDO 回读 0x2200:00
   → 恢复并验证 0x2200:00 原值
   → 本地主站保持 Operational，供后续自动流程继续使用
→ A04 SYNC PDO
   → 校验主站 0x1005 为 SYNC producer、从机 0x1005 为 SYNC consumer
   → 保存 TPDO1 communication parameters
   → 从机进入 Pre-operational，按 disable/type=1/enable 顺序临时切换 TPDO1
   → 无 SYNC 窗口确认 TPDO1 静默
   → Lely 主站周期发送 5 个 SYNC，验证每个 SYNC 对应 1 个 TPDO1
   → 停止 SYNC，恢复并回读 TPDO1 原参数
   → 无 SYNC 下验证恢复后的事件型 TPDO1 周期
→ A05 TIME Consumer
   → 保存、按测试需要调整并恢复从机 0x1012
   → 通过 Lely 发送可控 TIME，并用 MCU 0x2300 receive count/ms/days 验证接收与应用
→ A06 EMCY
   → 共享 OnEmcy 事件流验证 0x8130/0x0000、0x1001 和 0x1003
   → 0x1014 与主站本地 0x1028:01 按 disable/change/enable 联动切换到 0x681
   → 设置 0x1015=15000，按单调时间戳验证 1.5 s inhibit
   → 恢复 0x1014/0x1015/0x1028/主站 Heartbeat 并执行原 COB-ID smoke test
→ B06 EMCY Consumer
   → Host Node 127 通过 `EmcyTestMaster::pushLocalEmcy()` 发送确定性 EMCY
   → 通过 MCU 0x2301 诊断记录验证 source/COB-ID/EEC/ER/errorBit/infoCode
   → 重复 EMCY 必须逐帧增加 remote_rx_count
   → 通过 Lely 本地 EMCY clear 发送标准 0x0000 recovery
   → Reset Communication 后确认 0x2301 count/last snapshot 保持不变，再发送 fresh EMCY 验证 callback 重绑
→ J03/B09G GFC
   → AsyncMaster 通过 SDO 保存/切换/恢复 MCU 0x1300 valid 参数
   → 独立 Lely CanChannel 只注入/捕获固定 CAN-ID 0x001
   → 通过 MCU 0x2302 验证 consumer count、producer sequence/result 和 Reset Communication rebind
→ 等待 Ctrl+C
→ Final Reset Communication
```

A03 不使用外部 `cansend`，PDO 收发由 Lely `OnRpdo()`、`OnTpdo()`、`RpdoMapped()` 和 `TpdoMapped()` 完成。A04 同样不构造原始 CAN 帧，SYNC 由 Lely 本地 `0x1006` producer 定时器产生，并通过 `OnSync()` 与 `OnRpdo()` 建立同步时序证据。当前 EDS/DCF 已包含 A03/A04 所需的默认 PDO 和 SYNC 配置，不需要为该测试重新生成配置。

A05 TIME Consumer 实现保留，但当前 `CANOPEN_ENABLE_TIME_PROCESS=0`，因此不进入默认 Master 自动流程。

## 流程私有配置

公共 `include/canopen_config.h` 只保留运行环境、Node-ID、公共等待时间和流程开关。测试对象、probe value、采样数量、周期容差等流程专用参数保存在对应 `.cpp` 的匿名命名空间中：

- A01：`src/nmt_heartbeat.cpp`；
- A02：`src/sdo_process.cpp`；
- A03：`src/pdo_process.cpp`；
- A04：`src/sync_pdo_process.cpp`；
- A05：`src/time_process.cpp`；
- A06：`src/emcy_process.cpp`，共享 EMCY 事件层位于 `src/canopen_emcy.cpp`；
- B06：`src/emcy_consumer_process.cpp`，读取 MCU `0x2301` diagnostic；
- J03/B09G：`src/gfc_process.cpp`，通过 MCU `0x1300/0x2302` 与独立 fixed-ID wire channel 验证 GFC。

从机基线 Heartbeat 和 PDO 通信参数仍以 `config/master.yml`、EDS 和生成后的 DCF 为准。

## 关键文件

```text
config/master.yml                  dcfgen 输入
config/generated/master.dcf       主站运行 DCF
config/generated/mcu_node_1.bin   节点 1 concise DCF
include/nmt_heartbeat.h            A01/Boot 接口
include/sdo_process.h              A02 接口
include/pdo_process.h              A03 接口
include/sync_pdo_process.h         A04 接口
include/time_process.h             A05 TIME consumer 接口
include/canopen_emcy.h              共享 EMCY event observer
include/canopen_master.h            B06-only Lely local EMCY test shim
include/emcy_process.h              A06 EMCY producer 接口
include/emcy_consumer_process.h     B06 EMCY consumer 接口
include/gfc_process.h               J03/B09G GFC 接口
include/nmt_master_process.h        NMT Master 行为验证接口（Slave role）
src/canopen_emcy.cpp                唯一 OnEmcy callback、序列化缓存与时间戳
src/nmt_heartbeat.cpp              Boot、Heartbeat 和 A01
src/sdo_process.cpp                A02 用户 OD SDO 验证
src/pdo_process.cpp                A03 RPDO/TPDO 验证
src/sync_pdo_process.cpp           A04 SYNC consumer/同步 TPDO 验证
src/time_process.cpp               A05 TIME consumer 主机侧验证
src/emcy_process.cpp               A06 EMCY/0x1001/0x1003/0x1014/0x1015 验证
src/emcy_consumer_process.cpp      B06 Host EMCY producer/MCU 0x2301 验证
src/gfc_process.cpp                J03/B09G GFC fixed-ID wire/0x2302 验证
src/nmt_master_process.cpp         Lely BasicSlave NMT callback 顺序验证
src/main.cpp                       公共 Lely 生命周期、Master/Slave 角色入口
src/shutdown_process.cpp           Final Reset Communication
```

## 重新生成配置

在已安装匹配版本 `dcf-tools` 的环境中执行：

```sh
cd config

../.venv-dcf-tools/bin/dcfgen \
    -r \
    -v \
    -d generated \
    master.yml
```

A03/A04 当前直接使用既有 `project.eds`/`master.dcf` 映射和 SYNC 对象。A06 不永久修改 EDS/DCF；运行期临时同时修改从机 `0x1014` 和主站本地 `0x1028:01`，测试后恢复并回读。A04 只在运行期临时修改节点 1 的 `0x1800` 并恢复，不修改配置源。B06 不修改 Host/MCU EMCY 通信参数，只要求 MCU 固件提供只读 `0x2301:01..07` 诊断记录。只有基线通信参数或映射本身发生变化时才需要重新生成 DCF。

详细检查项见 [`docs/configuration.md`](docs/configuration.md) 和 [`docs/acceptance.md`](docs/acceptance.md)。

## 构建

首次准备本机交叉编译配置：

```sh
cp cmake/build_config.local.cmake.example cmake/build_config.local.cmake
```

修改 Yocto SDK、sysroot 和 Lely stage 路径后，普通构建直接使用默认 `MinSizeRel`：

```sh
rm -rf build
cmake -S . -B build \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --verbose -j"$(nproc)"
```

首次构建会同时把 vendored spdlog 1.17.0 编译为
`build/third_party/spdlog/libspdlog.a`，不需要手工生成或提交预编译 `.a`。

需要源码级调试时显式切换到 `Debug`：

```sh
rm -rf build
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --verbose -j"$(nproc)"
```

Debug 构建保留 `-O0 -g3 -fno-omit-frame-pointer -fno-optimize-sibling-calls`。

## 部署和运行

```sh
cmake --build build --target download
```

`deploy/run.sh` 会把可执行文件、`master.dcf`、`project.eds` 和 `mcu_node_1.bin` 上传到暂存文件，备份现有文件，再在目标目录内逐文件原子替换。Master/Slave 两种角色继续共用同一部署流程；Slave role 直接复用 MCU 提供的 `project.eds`，不修改其中的 NMT startup，也不需要额外 concise DCF。完成 Reset 并注册 NMT observer 后才把 Node 2 的 `0x1017` 配成 500 ms Heartbeat；如果 peer 当前不是 PRE-OP，MCU 会先发送额外 PREOP 做测试夹具归一化。Reset 完成后 Host 在内部 PRE-OP callback 处恢复 Heartbeat，MCU 再按需要归一化到 PRE-OP 后继续正式序列。

默认目标布局：

```text
/opt/Ultra/Debug/canopen-master/
├── bin/
│   └── canopen_master
└── config/
    ├── master.dcf
    ├── project.eds
    └── mcu_node_1.bin
```

程序从远端 `bin/` 目录启动，`CANOPEN_MASTER_DCF_PATH` 默认为 `../config/master.dcf`。

## 目标板观察

抓包：

```sh
candump -t A -x can1
```

正常流程应能观察到：

- `0x77F`：主站 Heartbeat；
- `0x701`：从机 Heartbeat；
- `0x601/0x581`：A01/A02/A03/A04/B06 使用的远端 SDO；
- `0x181`：从机 TPDO1，默认约 1000 ms 周期、DLC 8；
- `0x201`：A03 发送给从机的 RPDO1，DLC 4；
- `0x080`：A04 由 Lely 主站产生的 SYNC，当前测试为无 counter byte；
- A04 同步窗口内应表现为每个 `0x080` 后对应一个 `0x181`；
- `0x0FF`：B06 Host Node 127 产生的标准 EMCY，包括测试 error 与 `0x0000` recovery。

目标板验收必须同时检查程序日志、CAN 抓包和 `0x2200:00` 恢复结果；Host 语法检查不能替代目标板验证。
