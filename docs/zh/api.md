[English](../en/api.md)

# API 与模块映射

本工程不是对外提供稳定 library API 的库项目。`include/` 中的头文件主要是可执行程序运行框架与各协议验证模块之间的内部接口。

## 核心运行接口

| 接口 | 职责 |
| --- | --- |
| `canopen_config.h` | 共享 role、SocketCAN、Node-ID、timeout、日志配置 |
| `canopen_process.h` | 有序流程注册与执行契约 |
| `canopen_nmt.h` | NMT 命令提交和 callback 确认的状态迁移 |
| `canopen_sdo.h` | 公共有界 SDO read/write 与 block-transfer helper |
| `canopen_emcy.h` | 公共 EMCY event observer/cache |
| `canopen_master.h` | 测试需要的最小 `CanopenTestMaster` Lely service access |
| `shutdown_process.h` | Final Reset Communication cleanup |

## 协议验证模块

每个协议模块通过对应头文件暴露 process entry，并在同一头文件中拥有自己的 enable 宏。`src/main.cpp` 只负责按固定顺序注册已启用流程。

```text
nmt_heartbeat.h / nmt_heartbeat.cpp       Heartbeat
sdo_process.h / sdo_process.cpp           SDO object access
sdo_block_process.h / .cpp                SDO server block transfer
storage_process.h / .cpp                  Storage persistence
sdo_client_process.h / .cpp               MCU SDO client
pdo_process.h / .cpp                      PDO
sync_pdo_process.h / .cpp                 SYNC/synchronous PDO
time_process.h / .cpp                     TIME consumer
emcy_process.h / .cpp                     EMCY producer
emcy_consumer_process.h / .cpp            EMCY consumer
gfc_process.h / .cpp                      GFC
srdo_process.h / .cpp                     SRDO
nmt_master_process.h / .cpp               MCU NMT Master behavior
```

## 返回值语义

流程入口返回 `0` 表示该流程实际实现的断言和 cleanup 条件通过；非 0 表示失败。不能把单流程返回值扩展解释为它没有实现的证据，例如完整 bus-health monitoring 或系统级安全认证。

## 详细参考

旧版更详细的中文模块说明保留在 [`reference/api.md`](reference/api.md)。
