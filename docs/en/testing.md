[中文](../zh/testing.md)

# Testing and validation

## Evidence model

The Host process combines three kinds of evidence:

1. standard CANopen service results from Lely;
2. MCU Object Dictionary state read back through SDO;
3. narrowly scoped wire-level capture/injection for protocol frames not exposed by Lely high-level services.

A process returning PASS only proves the assertions implemented by that process. It does not automatically prove target timing margins, bus health for the full run, power-cycle behavior, or functional-safety certification.

## Validation capabilities

| Capability | Main evidence | Additional DUT requirement |
| --- | --- | --- |
| Heartbeat | Boot/Heartbeat callbacks, EMCY/SDO evidence | Heartbeat producer/consumer configured |
| SDO object access | upload/download/read-back | writable user OD entry used by the test |
| SDO server block transfer | explicit block upload/download, abort/recovery | MCU SDO server block support and test object |
| Storage persistence | OD commands, raw backup/CRC, reset/power-cycle workflow | CANopenNode storage backend and operator actions for destructive modes |
| MCU SDO client | MCU diagnostic control/status OD + Host SDO-server fixture | MCU SDO client diagnostic fixture |
| PDO | Lely PDO callbacks/mapped values + OD read-back | matching PDO mapping |
| SYNC/synchronous PDO | Lely SYNC producer/callback timing + TPDO evidence | SYNC consumer and synchronous TPDO support |
| TIME consumer | injected TIME frame + MCU diagnostic OD | TIME diagnostic record exposed by MCU |
| EMCY producer | Host EMCY observer + 0x1001/0x1003/0x1014/0x1015 | configurable/history features required by the process |
| EMCY consumer | Host local EMCY producer + MCU diagnostic OD | MCU EMCY consumer diagnostic record |
| GFC | MCU 0x1300/0x2302 + fixed CAN-ID 0x001 fixture | CANopenNode GFC consumer/producer diagnostic support |
| SRDO | MCU SRDO control/status + paired wire capture/fault cases | CANopenNode SRDO profile configured for the test |
| NMT master behavior | Lely `BasicSlave` state callbacks | MCU acts as NMT master and drives Node 2 |

## Current checked-in profile

Only SRDO validation is enabled by default among the automatic Master processes. Final Reset Communication cleanup is enabled. All other protocol processes are available in source but disabled until their compile-time switch is changed.

Before enabling several processes together, verify that they agree on the MCU firmware configuration, temporary OD values, role, and cleanup assumptions.

## Fail-fast and cleanup

The Master role executes enabled processes in the fixed table order. A failed process stops later validation. Each process is responsible for restoring temporary values or reporting that the environment is dirty when safe restoration cannot be proven.

Final Reset Communication is a separate shutdown action controlled by `CANOPEN_ENABLE_FINAL_RESET_PROCESS`.

## HIL evidence that remains external

Depending on the process, complete target acceptance may still require:

- `candump` or another independent CAN trace;
- confirmation that the controller never entered error-passive/bus-off;
- physical power-cycle or interrupted-write action;
- target-side memory/resource monitoring;
- firmware-side logs or diagnostic OD snapshots;
- timing measurements on the actual bus/target.

The TQ8MP CI/Release cross-build does not provide any of these HIL results.

## Protocol-specific pages

- [NMT master validation](nmt-master-test.md)
- [EMCY consumer validation](emcy-consumer-test.md)
- [GFC validation](gfc-test.md)
- [SRDO validation](srdo-test.md)
- [CiA 303-3 LED manual validation](cia303-led-test.md)

Detailed historical test notes are retained under the reference directories. They are supporting evidence, not a replacement for the maintained configuration tables in this document.
