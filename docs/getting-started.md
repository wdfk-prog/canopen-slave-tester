# 从空环境完成首次 P1 运行

本教程面向在 Ubuntu 构建主机上交叉编译，并将程序部署到 TQ8MP 的开发者。完成后应能看到 Node-ID 1 boot-up 成功，并通过 `Ctrl-C` 干净退出。

## 准备构建主机

需要以下输入：

- Yocto aarch64 交叉编译工具链和目标 sysroot；
- 已经为目标平台构建并 staged 的 Lely 2.4.0 头文件和动态库；
- Python 虚拟环境 `.venv-dcf-tools`，其中安装了与 Lely 版本兼容的 `dcfgen`；首次安装见 [dcfgen 安装与使用](dcfgen-setup.md)；
- CMake 3.16 或更高版本；
- 可访问目标板的 SSH/SCP；
- 目标板上存在 `can1`，并可配置为 1 Mbit/s。

确认 dcfgen：

```sh
cd ~/share/lely-imx8p/canopen-slave-tester
source .venv-dcf-tools/bin/activate
which dcfgen
dcfgen --help
```

`which dcfgen` 应指向：

```text
.../canopen-slave-tester/.venv-dcf-tools/bin/dcfgen
```

## 配置交叉编译路径

从工程根目录运行：

```sh
cp cmake/build_config.local.cmake.example cmake/build_config.local.cmake
```

编辑 `cmake/build_config.local.cmake`：

```cmake
set(CANOPEN_TOOLCHAIN_BIN_DIR
    "/path/to/sdk/sysroots/x86_64-sdk-linux/usr/bin/aarch64-target-linux"
    CACHE PATH "Cross-compilation tool directory" FORCE)
set(CANOPEN_TOOLCHAIN_PREFIX
    "aarch64-target-linux"
    CACHE STRING "Cross-compilation tool prefix" FORCE)
set(CANOPEN_SYSROOT
    "/path/to/sdk/sysroots/aarch64-target-linux"
    CACHE PATH "Target sysroot" FORCE)
set(CANOPEN_LELY_INCLUDE_DIR
    "/path/to/lely/stage/usr/include"
    CACHE PATH "Lely include directory" FORCE)
set(CANOPEN_LELY_LIBRARY_DIR
    "/path/to/lely/stage/usr/lib"
    CACHE PATH "Lely library directory" FORCE)
set(CACHED_IP_ADDR
    "192.168.1.100"
    CACHE STRING "Target board address for deployment" FORCE)
```

该文件是本机配置，不应提交到版本库。

## 配置 P1 主站网络

编辑 `config/master.yml`，确认基线字段：

```yaml
master:
  node_id: 127
  baudrate: 1000
  start: false
  start_nodes: false
  start_all_nodes: false
  reset_all_nodes: false
  stop_all_nodes: false

mcu_node_1:
  dcf: "generated/project.dcfgen.eds"
  node_id: 1
  boot: false
  mandatory: false
  reset_communication: true
```

这里的 `baudrate` 单位是 kbit/s。`tester.conf` 中的 `can.bitrate` 单位是 bit/s，因此对应值为 `1000000`。

`reset_communication: true` 是当前 P1 的推荐值。它允许 Lely 发送单帧广播：

```text
000#8200
```

使用 `false` 时，Lely 为避免复位 Node-ID 1，可能分别向 Node-ID 2～126 发送 125 帧定向复位，开启 MCU 逐帧 trace 时可能导致软件接收队列溢出。

## 生成 master.dcf

保持虚拟环境激活，从工程根目录运行：

```sh
cd config
dcfgen -r -v -d generated master.yml
cd ..
```

检查结果：

```sh
grep -n -A5 '^\[1F80\]' config/generated/master.dcf
grep -n -A5 '^\[1F81Value\]' config/generated/master.dcf
```

P1 基线应包含：

```ini
[1F80]
DefaultValue=0x0000000D

[1F81Value]
NrOfEntries=1
1=0x00000001
```

`1F81:01 = 0x00000001` 表示 Node-ID 1 已分配给主站，但不设置“避免 Reset Communication”位。

## 配置运行时

检查 `config/tester.conf`：

```ini
[can]
interface=can1
bitrate=1000000

[master]
node_id=127
dcf_path=/opt/Ultra/Debug/canopen-slave-tester/config/master.dcf

[slave]
node_id=1
eds_path=/opt/Ultra/Debug/canopen-slave-tester/config/project.eds
```

目标板路径由部署脚本创建。完整字段见 [配置手册](configuration.md)。

## 编译

从工程根目录运行：

```sh
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

确认产物：

```sh
test -x build/canopen_slave_tester
test -s build/canopen_slave_tester.map
```

CMake 会从 `liblely-coapp.pc` 读取 Lely 版本，并从 vendored 头文件读取 spdlog 版本。缺少必需库时，配置阶段直接失败。

## 准备目标板

目标板上配置 CAN 接口：

```sh
ip link set can1 down 2>/dev/null || true
ip link set can1 type can bitrate 1000000
ip link set can1 up
ip -details -statistics link show can1
```

CAN 总线两端应正确终端，所有节点必须使用同一标称波特率。

首次使用还需要安装 Lely 运行库，见 [构建与部署](deployment.md#安装目标板-lely-运行库)。

## 部署并运行

从构建主机运行：

```sh
cmake --build build --target download
```

部署目标会上传可执行文件、`tester.conf`、`master.dcf` 和 `project.eds`，然后在目标板启动程序。

也可以登录目标板手工执行：

```sh
cd /opt/Ultra/Debug/canopen-slave-tester
./bin/canopen_slave_tester --config config/tester.conf --check-config
./bin/canopen_slave_tester --config config/tester.conf
```

## 判断首次运行是否成功

应看到：

```text
configuration validated
NMT: sending command specifier 130 to node 0
Lely runtime is ready
event loop started
boot node=1 state=0x0 status=success
```

从机应看到：

```text
RX ID:000 ... 82 00
communication reset requested
TX ID:701 ... 00
```

按 `Ctrl-C` 后应看到：

```text
SIGINT received
stop requested
event-loop shutdown started
event loop stopped after ... tasks
runtime resources released
session=... lifecycle=stopped can_state=active can_errors=0 dropped_logs=0
```

最后读取 MCU CAN 统计，确认本次测试前后：

```text
Dropped.receive.packages 增量 = 0
```

当前版本的实测结论和原始证据见 [P0/P1 验收结果与证据](acceptance.md)。
