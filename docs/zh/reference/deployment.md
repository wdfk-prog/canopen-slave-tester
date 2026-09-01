# 构建与部署

## 构建

普通部署默认使用 `MinSizeRel`：

```sh
rm -rf build
cmake -S . -B build
cmake --build build --verbose -j"$(nproc)"
```

首次构建会自动生成 `build/third_party/spdlog/libspdlog.a` 并静态链接到 Host，
不需要单独编译或部署 spdlog 动态库。

输出：

```text
build/canopen_master
build/canopen_master.map
```

## CMake 部署目标

| target | 行为 |
| --- | --- |
| `download` | 上传可执行文件和完整 CANopen runtime 配置并直接运行 |
| `debug-deploy` | 要求 Debug 构建，上传 `.elf` 并启动 gdbserver |
| `debug` | 兼容别名，依赖 `debug-deploy` |

普通部署：

```sh
cmake --build build --target download
```

远程调试前必须显式重新配置为 `Debug`：

```sh
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --verbose -j"$(nproc)"
cmake --build build --target debug-deploy
```

## 默认远端布局

```text
/opt/Ultra/Debug/canopen-master/
├── bin/
│   ├── canopen_master
│   └── canopen_master.elf
└── config/
    ├── master.dcf
    ├── project.eds
    └── mcu_node_1.bin
```

`CANOPEN_MASTER_DCF_PATH` 默认为 `../config/master.dcf`，因此部署脚本必须从远端 `bin/` 目录启动程序。

Slave role 直接复用 `project.eds` 创建 `BasicSlave` Node 2，不加载额外 concise DCF；Master/Slave 两种角色继续共用同一个 `download`/`debug-deploy` 流程。

## 直接使用脚本

```sh
./deploy/run.sh canopen_master <target-ip> run build/canopen_master
./deploy/run.sh canopen_master <target-ip> gdb build/canopen_master
```

常用环境变量：

- `CANOPEN_TARGET_USER`
- `CANOPEN_TARGET_PASSWORD`
- `CANOPEN_TARGET_SSH_PORT`
- `CANOPEN_TARGET_PATH`
- `CANOPEN_TARGET_CONFIG_PATH`
- `CANOPEN_MASTER_DCF_PATH`
- `CANOPEN_PROJECT_EDS_PATH`
- `CANOPEN_MCU_NODE_DCF_PATH`
- `CANOPEN_GDB_PORT`

脚本不再上传运行时 INI 文件，也不会给程序传入参数。
