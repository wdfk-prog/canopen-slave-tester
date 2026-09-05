[中文](../zh/configuration.md)

# Configuration

## Shared runtime configuration

`include/canopen_config.h` contains only cross-module settings:

| Setting | Checked-in value | Meaning |
| --- | ---: | --- |
| `CANOPEN_ROLE` | `CANOPEN_ROLE_MASTER` | Host application role |
| `CANOPEN_INTERFACE_NAME` | `"can1"` | SocketCAN device |
| `CANOPEN_EXPECTED_BITRATE` | `1000000` | Expected nominal bitrate, bit/s |
| `CANOPEN_MASTER_NODE_ID` | `127` | Lely master Node-ID |
| `CANOPEN_SLAVE_NODE_ID` | `1` | MCU Node-ID |
| `CANOPEN_PEER_PROFILE` | `CANOPEN_PEER_PROFILE_LELY_RTT_B4` | Selects the BasicSlave fixture contract |
| `CANOPEN_PEER_NODE_ID` | `1` for the default profile | Lely `BasicSlave` peer Node-ID |
| `CANOPEN_PEER_HEARTBEAT_MS` | `1000` for the default profile | Software peer heartbeat period |
| `CANOPEN_WAIT_TIMEOUT_MS` | `5000` | Shared Boot/NMT wait budget |
| `CANOPEN_CHANNEL_RX_QUEUE_SIZE` | `256` | Lely CAN receive queue |
| `CANOPEN_LOG_QUEUE_SIZE` | `8192` | spdlog async queue |
| `CANOPEN_LOG_WORKER_COUNT` | `1` | spdlog worker count |

## BasicSlave peer profiles

`CANOPEN_ROLE_SLAVE` uses `CANOPEN_PEER_PROFILE` to select one of two compatible fixture contracts:

- `CANOPEN_PEER_PROFILE_LELY_RTT_B4` (default): Node 1, `config/lely_rtt_node1.dcf`, 1000 ms heartbeat, passive NMT transition callback tracing, and no fixed command-order assertion. Use this with `lely-canopen-rtt` B4 Master integration/HIL tests.
- `CANOPEN_PEER_PROFILE_CANOPENNODE_NMT`: Node 2, `config/project.eds`, 500 ms heartbeat, and the historical strict `nmtMasterProcess()` sequence. Select this profile before setting `CANOPEN_ENABLE_NMT_MASTER_PROCESS=1`.

The lely-rtt profile deliberately keeps `CANOPEN_ENABLE_NMT_MASTER_PROCESS=0`: Lely NMT boot may emit Reset Communication before a manually driven START/STOP/PREOP sequence, so a fixed callback order would create fixture false failures.

## Validation switches

The process-specific enable switch lives with each module interface. The checked-in values are:

| Capability | Macro | Default |
| --- | --- | ---: |
| Heartbeat | `CANOPEN_ENABLE_HEARTBEAT_PROCESS` | `0` |
| SDO object access | `CANOPEN_ENABLE_SDO_PROCESS` | `0` |
| SDO server block transfer | `CANOPEN_ENABLE_SDO_BLOCK_PROCESS` | `0` |
| Storage persistence | `CANOPEN_ENABLE_STORAGE_PROCESS` | `0` |
| MCU SDO client | `CANOPEN_ENABLE_SDO_CLIENT_PROCESS` | `0` |
| MCU SDO client block regression | `CANOPEN_ENABLE_SDO_CLIENT_BLOCK_REGRESSION` | `0` |
| PDO | `CANOPEN_ENABLE_PDO_PROCESS` | `0` |
| SYNC / synchronous PDO | `CANOPEN_ENABLE_SYNC_PDO_PROCESS` | `0` |
| TIME consumer | `CANOPEN_ENABLE_TIME_PROCESS` | `0` |
| EMCY producer | `CANOPEN_ENABLE_EMCY_PROCESS` | `0` |
| EMCY consumer | `CANOPEN_ENABLE_EMCY_CONSUMER_PROCESS` | `0` |
| GFC | `CANOPEN_ENABLE_GFC_PROCESS` | `0` |
| SRDO | `CANOPEN_ENABLE_SRDO_PROCESS` | `1` |
| NMT master behavior | `CANOPEN_ENABLE_NMT_MASTER_PROCESS` | `0` |
| Final Reset Communication | `CANOPEN_ENABLE_FINAL_RESET_PROCESS` | `1` |

Do not enable a process solely because the Host code exists. Match it to the MCU CANopenNode configuration and the required diagnostic OD objects.

## Lely DCF/EDS configuration

`config/master.yml` defines the Lely master and managed MCU node used by `dcfgen`. The checked-in baseline uses:

- master Node-ID `127`;
- MCU Node-ID `1`;
- bitrate `1000` kbit/s;
- master and MCU producer heartbeat `500` ms;
- heartbeat consumer enabled in both directions;
- automatic master start disabled;
- MCU boot/configuration enabled with Reset Communication allowed.

Generated files are stored under `config/generated/`. Regenerate them when the underlying EDS or baseline communication configuration changes.

## Target cross-build configuration

The default build loads `cmake/build_config.cmake`, which requires the developer-local `cmake/build_config.local.cmake`. The local file supplies Yocto toolchain, sysroot, Lely include/library paths, and the optional deployment address.

Do not commit machine-local paths or credentials. Both local configuration files are listed in `.gitignore`; if either is already tracked, remove it from the Git index separately.

## Optional native host build

Set `CANOPEN_NATIVE_BUILD=ON` only for an explicit local host build with a compatible native Linux Lely installation. CI does not build the target; the Release workflow uses the real TQ8MP cross toolchain.

Required variables:

```text
CANOPEN_LELY_INCLUDE_DIR
CANOPEN_LELY_LIBRARY_DIR
```

This optional host mode remains separate from the TQ8MP product build and is not release evidence.
