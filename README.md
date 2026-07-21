# CANopen 从机测试工具

本工程基于 Lely CANopen，实现阶段 P1 的 CANopen 主站运行时，用于连接和观察 MCU CANopenNode 从机。

当前程序支持：

- 加载并严格校验 `config/tester.conf`；
- 打开 SocketCAN 接口并校验标称波特率和控制器状态；
- 创建 Lely I/O 上下文、事件循环、定时器、CAN 通道和 `AsyncMaster`；
- 按配置路径加载任意合法、非空的 master DCF；
- 记录 CAN 状态、CAN 错误、heartbeat、NMT 状态和 boot-up 事件；
- 使用 vendored Header-only spdlog 输出有界异步日志；
- 通过 `SIGINT` 或 `SIGTERM` 请求安全退出；
- 支持同一 `LelyRuntime` 对象执行多轮 `Start()`/`Stop()`，启动失败时自动回滚半初始化资源；
- 使用稳定退出码区分配置、SocketCAN、Lely 和内部错误。

当前阶段不包含 replxx、测试 Harness 以及 NMT、SDO、PDO 自动测试用例。

## 构建配置

机器相关路径不写入版本库。首次构建时复制本地配置模板：

```sh
cp cmake/build_config.local.cmake.example cmake/build_config.local.cmake
```

然后在 `cmake/build_config.local.cmake` 中设置 Yocto SDK、sysroot、Lely staged 头文件和库目录。
该文件已由 `.gitignore` 排除；`cmake/build_config.cmake` 只负责加载并校验本地配置。

P1 构建直接使用已有 Lely staged 产物，不会解压、复制或重新安装 Lely。修改交叉编译器、目标架构、sysroot 或 Lely 路径后，需要删除旧构建目录：

```sh
rm -rf build
```

## 编译

```sh
mkdir -p build
cd build
cmake ..
make -j"$(nproc)"
```

不需要设置 `CROSS_COMPILE`、`LELY_STAGE`，也不需要传入 `CMAKE_TOOLCHAIN_FILE`。

默认生成：

```text
build/canopen_slave_tester
build/canopen_slave_tester.map
```

## spdlog 集成

工程以 Header-only 方式导入 vendored spdlog 源码：

```text
third_party/spdlog/include/spdlog
```

源码随本工程交叉编译，不生成或链接独立 `libspdlog.so`、`libspdlog.a`。

## 日志配置

日志等级和输出策略位于 `config/tester.conf` 的 `[logging]` 段。常用调试配置：

```ini
runtime_level=debug
socketcan_level=debug
canopen_level=debug
console_level=info
file_level=debug
```

异步队列固定采用 `overrun_oldest`，队列满时覆盖最旧消息，不阻塞 Lely event loop。

## DCF 配置

DCF 由配置文件决定，不与可执行程序哈希绑定：

```ini
[master]
dcf_path=/opt/Ultra/Debug/canopen-slave-tester/config/master.dcf
```

本地生成文件为 `config/generated/master.dcf`。部署脚本会将其安装为目标板上的
`/opt/Ultra/Debug/canopen-slave-tester/config/master.dcf`。切换 DCF 只需修改部署文件或配置路径并重新启动程序。

## 部署与远程调试

在 `cmake/build_config.local.cmake` 中设置目标板地址：

```cmake
set(CACHED_IP_ADDR
    "192.168.1.100"
    CACHE STRING "Target board address for deployment" FORCE)
```

该本地配置使用 `CACHE ... FORCE`，因此不要使用 `-DCACHED_IP_ADDR=...` 临时覆盖；修改文件后重新配置：

```sh
cmake -S . -B build
```

部署并直接运行：

```sh
cmake --build build --target download
```

部署并启动 `gdbserver`：

```sh
cmake --build build --target debug
```

部署脚本默认使用 SSH 密钥认证。需要密码认证时，先导出：

```sh
export CANOPEN_TARGET_PASSWORD='your-password'
```

`download` 和 `debug` 每次都会上传并替换以下文件：

| 本地文件 | 目标板文件 |
| --- | --- |
| 构建生成的可执行文件 | `bin/canopen_slave_tester`；`debug` 模式为 `bin/canopen_slave_tester.elf` |
| `config/tester.conf` | `config/tester.conf` |
| `config/generated/master.dcf` | `config/master.dcf` |
| `config/project.eds` | `config/project.eds` |

三个配置文件是程序运行的必需输入，部署前脚本会检查其存在且非空。目标板已有文件会分别备份到
`bin/backup/` 和 `config/backup/`。

远端激活按文件顺序执行，不提供四个文件整体的事务式回滚；该行为面向配置保持兼容的开发调试流程。
任一步骤失败时，脚本会返回非零退出码，并在当前终端输出远端标准错误及阶段提示，例如
`Target preparation failed`、`upload failed`、`Deployment activation failed` 或 `Remote execution failed`。
需要恢复时，可根据终端错误阶段从对应 `backup/` 目录手动还原最近备份。

SSH 主机密钥在首次连接时由 `ssh-keyscan` 自动写入 `known_hosts`，该流程仅适用于受信任的隔离调试网络。

### 安装目标板 Lely 运行库

目标板地址和宿主机上的 Lely 归档路径属于本地环境信息，不写入脚本或版本库。首次使用时复制部署配置模板：

```sh
cp deploy/local.conf.example deploy/local.conf
chmod 600 deploy/local.conf
```

在 `deploy/local.conf` 中设置：

```sh
CANOPEN_TARGET_IP="192.0.2.10"
CANOPEN_LOCAL_LELY_ARCHIVE="/path/to/lely-runtime.tar.gz"
CANOPEN_TARGET_USER="root"
CANOPEN_TARGET_SSH_PORT="22"
CANOPEN_SSH_CONNECT_TIMEOUT="5"
CANOPEN_TARGET_PASSWORD=""
```

`CANOPEN_LOCAL_LELY_ARCHIVE` 是宿主机上的归档绝对路径。脚本会通过 SCP 将归档上传到目标板临时
目录，然后在目标板解压并安装运行库。`deploy/local.conf` 已由 `.gitignore` 排除。密码可保存在
该本地文件中，但仍优先推荐 SSH 密钥。配置完成后直接执行，不需要任何位置参数：

```sh
./deploy/install_lely.sh
```

部署脚本的提示、错误和远端安装输出统一使用英文，并直接输出到当前终端，不保存本地日志文件。

脚本自动定位归档中包含完整 Lely 运行库集合的目录，将 `liblely-*.so*` 安装到
`/usr/local/lib`，写入 `/etc/ld.so.conf.d/lely.conf` 并执行 `ldconfig`。安装前会备份已有版本，
失败时尝试自动回滚，并清理上传到目标板的临时归档；若回滚未完整完成，脚本会明确提示并保留备份目录供手工恢复。程序依赖检查不属于该脚本职责。正式产品建议
将 Lely 制作成 Yocto/DEB 软件包，由系统包管理器安装和升级。

如果脚本报告 `The SSH service is not accepting connections`，说明目标地址的 SSH 端口没有接受连接。
应检查目标板 IP、网线或路由、防火墙，以及 `sshd` 或 `dropbear` 是否已启动；若 SSH 使用非 22
端口，在 `deploy/local.conf` 中修改 `CANOPEN_TARGET_SSH_PORT`。

## 目标板运行

将可执行程序和配置部署到目标目录后，先检查配置：

```sh
./canopen_slave_tester --config config/tester.conf --check-config
```

查看程序版本：

```sh
./canopen_slave_tester --version
```

启动运行时：

```sh
./canopen_slave_tester --config config/tester.conf
```

按 `Ctrl-C` 请求正常退出。
