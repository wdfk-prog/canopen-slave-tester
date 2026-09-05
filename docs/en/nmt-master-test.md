[Chinese](../zh/nmt-master-test.md)

# NMT master validation and slave peer profiles

The Linux application can run as `CANOPEN_ROLE_SLAVE` with one of two Lely `BasicSlave` fixture profiles. The profiles are intentionally separate because the historical CANopenNode NMT test expects a fixed command sequence, while `lely-canopen-rtt` B4 performs its own NMT boot/reset flow.

## lely-canopen-rtt B4 integration profile

Use the checked-in defaults below when validating `lely-canopen-rtt` PR #4 style Master behavior:

```c
#define CANOPEN_ROLE CANOPEN_ROLE_SLAVE
#define CANOPEN_PEER_PROFILE CANOPEN_PEER_PROFILE_LELY_RTT_B4
#define CANOPEN_ENABLE_NMT_MASTER_PROCESS 0
```

This profile creates Node 1 from `config/lely_rtt_node1.dcf`, matching the B4 fixture identity, predefined SDO connection, 1000 ms Producer Heartbeat, and test objects `0x2000`/`0x2001`. `BasicSlave::OnCommand()` is registered only as a passive trace. It logs START, STOP, PREOP, RESET_NODE, and RESET_COMM but does not enforce their order.

This mode supports the HIL evidence chain used by the RT-Thread port test plan:

- MCU-side `co nmt ...`, `co node 1`, and `co boot 1` results;
- Host passive NMT transition callback logs;
- `candump` wire evidence;
- normal SDO upload/download/abort and heartbeat loss/recovery tests.

A deterministic active-SDO cancellation test still requires an SDO-response stall/fault-injection seam. The passive peer does not block the Lely event loop to fake this condition because doing so would also prevent the NMT reset/stop command from being processed.

## Legacy CANopenNode strict NMT sequence

For the existing CANopenNode NMT-master validation, select the legacy peer profile and enable the sequence checker:

```c
#define CANOPEN_ROLE CANOPEN_ROLE_SLAVE
#define CANOPEN_PEER_PROFILE CANOPEN_PEER_PROFILE_CANOPENNODE_NMT
#define CANOPEN_ENABLE_NMT_MASTER_PROCESS 1
```

The legacy profile keeps Node 2, `config/project.eds`, a 500 ms heartbeat, and the original fixed callback sequence. `nmtMasterProcess()` owns the single `BasicSlave::OnCommand()` slot in this mode, so passive tracing is disabled automatically.

## Evidence boundary

A Lely `BasicSlave` callback proves that the peer stack entered an NMT transition path; it does not by itself prove that a matching NMT command frame was received. Use `candump` as the wire-level command evidence. Complete HIL acceptance must still correlate MCU state, Host logs, and CAN frames. Host execution alone does not prove RT-Thread owner-thread lifecycle, target CAN timing, or shutdown races.

The earlier detailed design/test note is retained at `../zh/reference/CANopen_NMT_Master_Test.md`.
