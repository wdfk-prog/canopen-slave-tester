[中文](../zh/api.md)

# API and module map

This project does not expose a stable public library API. The headers under `include/` are internal interfaces between the executable runtime and validation modules.

## Core runtime

| Interface | Responsibility |
| --- | --- |
| `canopen_config.h` | shared role, SocketCAN, node, timeout, and logging configuration |
| `canopen_process.h` | ordered process registration/execution contract |
| `canopen_nmt.h` | NMT command submission and callback-confirmed state transitions |
| `canopen_sdo.h` | shared bounded SDO read/write and block-transfer helpers |
| `canopen_emcy.h` | shared EMCY event observer/cache |
| `canopen_master.h` | minimal `CanopenTestMaster` access used by test-only Lely services |
| `shutdown_process.h` | final Reset Communication cleanup |

## Validation modules

Each protocol module exposes one process entry function and owns its compile-time enable macro in the matching header. `src/main.cpp` only registers enabled processes and preserves their fixed order.

Examples:

```text
nmt_heartbeat.h / nmt_heartbeat.cpp       Heartbeat validation
sdo_process.h / sdo_process.cpp           SDO object-access validation
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
nmt_master_process.h / .cpp               MCU NMT-master behavior
```

## Contract rule

A process entry returning `0` means its implemented assertions and cleanup conditions passed. Non-zero means failure. Do not treat the return code as evidence for validations the process does not implement, such as complete bus-health monitoring or system-level safety certification.

## Detailed reference

A more detailed earlier Chinese module note is retained at `../zh/reference/api.md`.
