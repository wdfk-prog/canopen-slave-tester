[English](../en/getting-started.md)

# 快速开始

## 前置环境

实际部署到 TQ8MP 时需要：

- 项目使用的 Yocto SDK 与 aarch64 交叉编译器；
- 已面向目标架构构建的 Lely，包含头文件、共享库和 `liblely-coapp.pc`；
- CMake 3.16 或更高版本；
- Linux 开发机，以及项目实际使用的目标板访问方式和 CAN 工具。

## 1. 准备本机交叉编译配置

复制示例文件，只修改本机副本：

```sh
cp cmake/build_config.local.cmake.example cmake/build_config.local.cmake
```

根据开发机实际环境填写：

- `CANOPEN_TOOLCHAIN_BIN_DIR`
- `CANOPEN_TOOLCHAIN_PREFIX`
- `CANOPEN_SYSROOT`
- `CANOPEN_LELY_INCLUDE_DIR`
- `CANOPEN_LELY_LIBRARY_DIR`
- `CACHED_IP_ADDR`

`cmake/build_config.local.cmake` 已加入 `.gitignore`，用于避免新文件误提交；如果文件已被 Git 跟踪，还需单独从 Git index 中移除。

## 2. 构建 TQ8MP 版本

默认 CMake 路径会自动加载 `cmake/build_config.cmake`，再由它读取本机配置。

```sh
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --parallel
```

单配置生成器默认使用 `MinSizeRel`。源码级调试可使用：

```sh
cmake -S . -B build-debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-debug --parallel
```

## 3. 准备 SocketCAN 与 DUT

当前编译配置约定：

- Host CAN 接口：`can1`；
- 期望 bitrate：`1000000` bit/s；
- Host Master Node-ID：`127`；
- MCU Node-ID：`1`；
- Slave role 使用的软件 peer Node-ID：`2`。

程序启动 Lely 前会校验 CAN bitrate，但不会替系统创建或 UP `can1`。

## 4. 选择验证配置

`include/canopen_config.h` 选择 Host 角色；每个协议模块在自己的头文件中提供 enable 宏。当前仓库默认是 Master role，仅启用 SRDO 协议验证，其他 Master 自动验证流程默认关闭。

只有 MCU 固件实际支持对应 CANopen 功能，并提供该流程依赖的 test-only diagnostic OD 时，才应打开相应验证流程。

详见[配置说明](configuration.md)与[测试与验证](testing.md)。

## 5. 部署

需要脚本部署时先创建开发机私有配置：

```sh
cp deploy/local.conf.example deploy/local.conf
```

随后在目标交叉构建目录执行：

```sh
cmake --build build --target download
```

在使用密码 SSH 或修改目标板安装目录前，请先阅读[部署](deployment.md)。

## 可选 Native Host 构建

`CANOPEN_NATIVE_BUILD=ON` 仅保留给明确需要的本机 Host 侧实验，并要求存在兼容的 native Lely 安装。CI 仅执行 Cppcheck；Release 使用 [CI/CD](ci-cd.md) 中说明的 self-hosted runner 和真实 TQ8MP Yocto SDK。
