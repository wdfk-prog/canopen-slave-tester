# CANopen Master 双向 Heartbeat 专项测试

本工程基于 Lely CANopen，面向 TQ8MP Linux/aarch64 主站与 RT-Thread + CANopenNode MCU 从机。当前运行内容收敛为双向 Heartbeat 专项测试和正常退出流程，不执行独立 SDO、NMT 状态断言、PDO、SYNC 或独立 EMCY 测试。

## 测试链路

启动后，Lely 根据 `config/generated/master.dcf` 执行节点 1 的 Boot，并自动下载 `mcu_node_1.bin` 中的配置：

- 从机 `0x1016:01 = 0x007F05DC`：监控主站 Node-ID 127，超时 1500 ms；
- 从机 `0x1017:00 = 500`：每 500 ms 发送 Producer Heartbeat；
- 主站 `0x1016:01 = 0x000105DC`：监控从机 Node-ID 1，超时 1500 ms；
- 主站 `0x1017:00 = 500`：每 500 ms 发送 Producer Heartbeat。

自动测试流程：

```text
Boot 成功
→ 等待双向 Heartbeat 激活
→ 停止主站 Producer Heartbeat
→ 等待节点 1 的 0x8130 EMCY
→ 恢复主站 Producer Heartbeat
→ 等待节点 1 的 0x0000 EMCY reset
→ 向节点 1 发送 NMT Start
→ 主站通过 SDO 写节点 1 的 0x1017:00 = 0
→ 等待 OnHeartbeat(node=1, true)
→ 主站通过 SDO 写节点 1 的 0x1017:00 = 500
→ 等待 OnHeartbeat(node=1, false)
→ 保持运行直到 Ctrl+C
```

程序不使用 `AsyncRead()`、`AsyncWrite()`、`master.Read()` 或 `OnState()`。节点 1 的基线 Heartbeat 配置仍由 Lely Boot 和 concise DCF 管理；自动测试阶段仅使用 `SubmitWrite()` 临时将从机 `0x1017:00` 写为 0/500，并在测试结束后恢复正常配置语义。节点重新启动后，Lely Boot 仍自动重新执行 concise DCF。

## 关键文件

```text
config/master.yml                  dcfgen 输入
config/generated/master.dcf       主站运行 DCF
config/generated/mcu_node_1.bin   节点 1 concise DCF
include/nmt_heartbeat.h            Boot/Heartbeat 测试接口
src/nmt_heartbeat.cpp              Boot、Heartbeat、EMCY 回调和自动流程
src/main.cpp                       Lely 生命周期、启动、持续运行和退出编排
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

正常运行时应持续观察到 `0x77F` 和 `0x701`。自动测试第二阶段会出现两次远端 SDO 下载：先写节点 1 `0x1017:00 = 0` 触发主站 Heartbeat timeout，再写回 `500` 触发 recovery。测试结束后程序保持运行，等待 `Ctrl+C` 执行 Final Reset Communication。
