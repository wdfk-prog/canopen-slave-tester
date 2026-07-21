# 构建、安装和部署

## 构建输入

工程不下载或重建 Lely。CMake 直接使用本机已经 staged 的 Lely 头文件和动态库：

```text
liblely-coapp.so
liblely-io2.so
liblely-ev.so
liblely-co.so
liblely-can.so
liblely-util.so
liblely-libc.so
```

`CMakeLists.txt` 会检查每个文件，并从 `pkgconfig/liblely-coapp.pc` 提取版本。spdlog 以 Header-only 方式随工程交叉编译。

## 在构建主机部署 dcfgen

`dcfgen` 是 Ubuntu 构建主机工具，不安装到 TQ8MP。它把 `config/master.yml` 和从机 EDS/DCF 转换为 `config/generated/master.dcf`；目标板只加载部署后的 `config/master.dcf`。

首次创建 `.venv-dcf-tools`、从匹配版本的 Lely 源码安装 dcf-tools、验证环境、生成 DCF、比较校验值和处理离线 wheel，见 [dcfgen 安装与使用](dcfgen-setup.md)。

重新生成 DCF 后，必须再次执行部署，否则目标板会继续加载旧文件。

## 配置本机路径

```sh
cp cmake/build_config.local.cmake.example cmake/build_config.local.cmake
```

必须设置：

- `CANOPEN_TOOLCHAIN_BIN_DIR`
- `CANOPEN_TOOLCHAIN_PREFIX`
- `CANOPEN_SYSROOT`
- `CANOPEN_LELY_INCLUDE_DIR`
- `CANOPEN_LELY_LIBRARY_DIR`

可选设置目标板地址：

- `CACHED_IP_ADDR`

这些值以 `CACHE ... FORCE` 写入，因此不要期望 `-D` 临时参数覆盖文件中的值。修改后重新配置：

```sh
cmake -S . -B build
```

工具链/sysroot/Lely 路径变化时删除旧构建目录：

```sh
rm -rf build
```

## 编译

```sh
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

CMake 默认使用 `RelWithDebInfo`，C++14，并启用严格警告。输出：

```text
build/canopen_slave_tester
build/canopen_slave_tester.map
```

## 安装目标板 Lely 运行库

首次使用复制本地部署配置：

```sh
cp deploy/local.conf.example deploy/local.conf
chmod 600 deploy/local.conf
```

示例：

```sh
CANOPEN_TARGET_IP="192.168.1.100"
CANOPEN_LOCAL_LELY_ARCHIVE="/absolute/path/to/lely-runtime.tar.gz"
CANOPEN_TARGET_USER="root"
CANOPEN_TARGET_SSH_PORT="22"
CANOPEN_SSH_CONNECT_TIMEOUT="5"
CANOPEN_TARGET_PASSWORD=""
```

运行：

```sh
./deploy/install_lely.sh
```

脚本行为：

1. 验证本地归档可读且为有效 gzip tar；
2. 创建目标板安全临时目录；
3. 上传并解压归档；
4. 自动定位包含完整 Lely 动态库集合的目录；
5. 备份 `/usr/local/lib` 中旧版本；
6. 安装新库、写入 `/etc/ld.so.conf.d/lely.conf` 并执行 `ldconfig`；
7. 失败时尝试回滚并保留诊断信息。

该流程面向开发板。正式产品应使用系统包管理器。

## 配置 SocketCAN

目标板运行：

```sh
ip link set can1 down 2>/dev/null || true
ip link set can1 type can bitrate 1000000
ip link set can1 up
ip -details -statistics link show can1
```

程序会读取实际标称波特率，若不是 `tester.conf` 中的值，会以退出码 `3` 失败。

## 部署文件

`download` 和 `debug` 目标上传：

| 本地文件                      | 目标板文件                     |
| ----------------------------- | ------------------------------ |
| `build/canopen_slave_tester`  | `bin/canopen_slave_tester`     |
| 调试可执行文件                | `bin/canopen_slave_tester.elf` |
| `config/tester.conf`          | `config/tester.conf`           |
| `config/generated/master.dcf` | `config/master.dcf`            |
| `config/project.eds`          | `config/project.eds`           |

默认部署根目录：

```text
/opt/Ultra/Debug/canopen-slave-tester
```

目标板已有文件会分别备份到 `bin/backup/` 和 `config/backup/`。激活按文件顺序执行，不提供四个文件整体事务回滚。

## 一键部署和运行

```sh
cmake --build build --target download
```

需要密码认证时，优先在安全环境中临时导出：

```sh
export CANOPEN_TARGET_PASSWORD='your-password'
```

建议使用 SSH 密钥。首次连接时脚本通过 `ssh-keyscan` 写入主机密钥，仅适用于受信任的隔离开发网络。

## 远程调试

```sh
cmake --build build --target debug
```

该目标上传带调试信息的可执行文件，并在目标板启动 `gdbserver`。宿主机 GDB 应使用同一 Yocto SDK 的交叉调试器和 sysroot。

## 手工运行

```sh
ssh root@TARGET_IP
cd /opt/Ultra/Debug/canopen-slave-tester
./bin/canopen_slave_tester --version
./bin/canopen_slave_tester --config config/tester.conf --check-config
./bin/canopen_slave_tester --config config/tester.conf
```

## 部署后完整性检查

构建主机：

```sh
sha256sum build/canopen_slave_tester \
  config/tester.conf \
  config/generated/master.dcf \
  config/project.eds
```

目标板：

```sh
cd /opt/Ultra/Debug/canopen-slave-tester
sha256sum bin/canopen_slave_tester \
  config/tester.conf \
  config/master.dcf \
  config/project.eds
```

将校验值保存到验收记录中，避免因旧 DCF 或旧可执行文件造成误判。
