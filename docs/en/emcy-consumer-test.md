[中文](../zh/emcy-consumer-test.md)

# EMCY consumer validation

This process validates the direction opposite to the normal MCU EMCY-producer test:

```text
Host local EMCY producer (Node 127)
    -> CAN EMCY frame
    -> MCU CANopenNode EMCY consumer
    -> MCU diagnostic OD 0x2301
    -> Host SDO read-back assertions
```

## Preconditions

The MCU firmware must enable its EMCY consumer and expose the project diagnostic record at `0x2301`. The Host local EMCY producer must start from a clean active-error stack so the test cannot accidentally erase a pre-existing Host fault.

## Coverage

The implementation verifies:

- single EMCY delivery and manufacturer-specific information;
- consecutive different EMCY messages;
- duplicate EMCY delivery without host-side de-duplication assumptions;
- standard `0x0000` recovery delivery;
- ordinary SDO health after EMCY callbacks;
- Reset Communication followed by callback/diagnostic rebind evidence;
- count/snapshot consistency and cleanup ownership.

## Cleanup rule

The process only clears EMCY entries it can prove it created. If the Host local EMCY stack no longer matches the expected test-owned errors, cleanup is refused and the process remains failed rather than clearing unrelated faults.

## HIL boundary

A process PASS is based on SDO diagnostic snapshots and Lely/Boot evidence. Independent CAN-state logging or `candump` is still required when acceptance needs proof that the bus never entered error-passive/bus-off.

## Detailed reference

The earlier detailed Chinese note is retained at `../zh/reference/CANopen_EMCY_Consumer_Test.md`.
