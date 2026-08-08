# CANopen Master 自动协议测试

本工程基于 Lely CANopen，面向 TQ8MP Linux/aarch64 主站与 RT-Thread + CANopenNode MCU 从机。当前自动流程按顺序执行 A01 Heartbeat、A02 SDO 和 A03 RPDO/TPDO；任一流程失败后停止执行后续自动流程。测试结束后程序继续运行，直到收到 `Ctrl+C`/`SIGTERM`，再执行 Final Reset Communication 并退出。

## 自动测试链路

启动后，Lely 根据 `config/generated/master.dcf` 执行节点 1 的 Boot，并自动下载 `mcu_node_1.bin` 中的 Heartbeat 配置：

- 从机 `0x1016:01 = 0x007F05DC`：监控主站 Node-ID 127，超时 1500 ms；
- 从机 `0x1017:00 = 500`：每 500 ms 发送 Producer Heartbeat；
- 主站 `0x1016:01 = 0x000105DC`：监控从机 Node-ID 1，超时 1500 ms；
- 主站 `0x1017:00 = 500`：每 500 ms 发送 Producer Heartbeat。

当前自动流程：

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
→ 等待 Ctrl+C
→ Final Reset Communication
```

A03 不使用外部 `cansend`，PDO 收发由 Lely `OnRpdo()`、`OnTpdo()`、`RpdoMapped()` 和 `TpdoMapped()` 完成。当前 EDS/DCF 已包含 A03 所需的默认 PDO 映射，不需要为该测试重新生成配置。

## 流程私有配置

公共 `include/canopen_config.h` 只保留运行环境、Node-ID、公共等待时间和流程开关。测试对象、probe value、采样数量、周期容差等流程专用参数保存在对应 `.cpp` 的匿名命名空间中：

- A01：`src/nmt_heartbeat.cpp`；
- A02：`src/sdo_process.cpp`；
- A03：`src/pdo_process.cpp`。

从机基线 Heartbeat 和 PDO 通信参数仍以 `config/master.yml`、EDS 和生成后的 DCF 为准。

## 关键文件

```text
config/master.yml                  dcfgen 输入
config/generated/master.dcf       主站运行 DCF
config/generated/mcu_node_1.bin   节点 1 concise DCF
include/nmt_heartbeat.h            A01/Boot 接口
include/sdo_process.h              A02 接口
include/pdo_process.h              A03 接口
src/nmt_heartbeat.cpp              Boot、Heartbeat、EMCY 和 A01
src/sdo_process.cpp                A02 用户 OD SDO 验证
src/pdo_process.cpp                A03 RPDO/TPDO 验证
src/main.cpp                       Lely 生命周期和自动流程注册
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

A03 当前直接使用既有 `project.eds`/`master.dcf` 映射；只有通信参数或映射本身发生变化时才需要重新生成 DCF。

详细检查项见 [`docs/configuration.md`](docs/configuration.md) 和 [`docs/acceptance.md`](docs/acceptance.md)。

## 构建

首次准备本机交叉编译配置：

```sh
cp cmake/build_config.local.cmake.example cmake/build_config.local.cmake
```

修改 Yocto SDK、sysroot 和 Lely stage 路径后执行：

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

`deploy/run.sh` 会把可执行文件、`master.dcf`、`project.eds` 和 `mcu_node_1.bin` 上传到暂存文件，备份现有文件，再在目标目录内逐文件原子替换。

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
- `0x601/0x581`：A01/A02/A03 使用的远端 SDO；
- `0x181`：从机 TPDO1，默认约 1000 ms 周期、DLC 8；
- `0x201`：A03 发送给从机的 RPDO1，DLC 4。

目标板验收必须同时检查程序日志、CAN 抓包和 `0x2200:00` 恢复结果；Host 语法检查不能替代目标板验证。
