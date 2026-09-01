[English](../en/deployment.md)

# 部署

## 本机部署配置

先从示例创建本机配置：

```sh
cp deploy/local.conf.example deploy/local.conf
```

`deploy/local.conf` 可以保存目标板地址、SSH 用户/端口以及可选密码认证。该文件已加入 `.gitignore`；如果已有副本被 Git 跟踪，还需要单独从 Git index 中移除。条件允许时优先使用 SSH key；如果必须保存本地密码，应限制文件权限。

## CMake 部署 target

部署 helper 使用默认 TQ8MP 交叉编译模式。CI 与 Release 在 self-hosted TQ8MP runner 上复用同一套目标构建契约。

完成目标交叉构建后：

```sh
cmake --build build --target download
```

Debug + gdbserver：

```sh
cmake --build build-debug --target debug-deploy
```

`debug-deploy` 会先检查当前构建配置确实是 `Debug`，避免拿优化版本进入源码调试流程。

## 安装内容

CMake install payload 包括：

```text
bin/canopen_master
config/project.eds
config/master.yml
config/master.dcf
config/mcu_node_1.bin
config/generated/master.bin
config/generated/project.dcfgen.eds
```

程序通过相对路径读取 `CANOPEN_MASTER_DCF_PATH`，因此远端 `bin/` 与 `config/` 的相对布局不能随意打乱。部署脚本还可能根据本机配置使用项目既有目标目录；覆盖已有安装前应确认真实远端路径。

## Lely 运行库

目标程序链接交叉构建时使用的 Lely shared libraries。`deploy/install_lely.sh` 和 `deploy_lely_libraries.sh` 用于辅助目标端 Lely 部署。

Release 包会复制 self-hosted TQ8MP runner 所配置的同一套目标架构 Lely shared libraries；应保证 runner 的 Lely stage 与实际目标板部署环境一致。如果不把 Lely 安装到系统库目录，可在 Release 解压目录进入 `bin/` 后使用 `LD_LIBRARY_PATH=../lib ./canopen_master` 运行。

## 运行前检查

启动目标程序前至少确认：

1. 物理 CAN 总线与终端电阻正常；
2. `can1` 已按 1 Mbit/s 配置并 UP；
3. MCU 固件与当前启用验证流程匹配；
4. 生成的 DCF/EDS 与 MCU Object Dictionary 一致；
5. 如果验收涉及协议时序或 wire evidence，先准备独立 `candump` 记录。
