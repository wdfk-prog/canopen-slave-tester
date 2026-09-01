[中文](../zh/cia303-led-test.md)

# CiA 303-3 LED manual validation

The repository retains a detailed manual `coctl`-based CiA 303-3 LED validation record. This is a manual/operator workflow rather than one of the automatic `canopen_master` validation processes.

## Purpose

Use the procedure to correlate MCU LED behavior with CANopen communication/error state while exercising the expected NMT/communication conditions from a host tool.

## Recommended evidence

For each manual case record:

- the host command that created the state;
- relevant CAN trace;
- MCU NMT/communication/error state;
- observed RUN/ERROR LED pattern and timing;
- restoration step before the next case.

## Boundary

Visual LED observation is a hardware/HIL result. CI cross-builds or source inspection cannot validate LED timing or electrical behavior.

## Detailed reference

The full earlier Chinese operator procedure is retained at `../zh/reference/CANopen_CiA303-3_LED_coctl_test.md`.
