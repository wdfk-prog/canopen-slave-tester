[中文](../zh/gfc-test.md)

# GFC protocol validation

The GFC process validates CANopenNode Global Fail-safe Command behavior. Lely does not expose a high-level GFC service in the API used by this project, so the Host keeps protocol control on the normal `AsyncMaster` channel and uses a second Lely `CanChannel` only for fixed CAN-ID `0x001` wire evidence.

## MCU diagnostic contract

The process uses the MCU GFC configuration/diagnostic objects around `0x1300` and `0x2302` to observe consumer/producer state through normal SDO access.

## Coverage

- preflight and baseline capture;
- valid consumer delivery when enabled;
- disabled consumer gate;
- invalid DLC rejection;
- producer wire format (`0x001`, DLC 0);
- consecutive GFC delivery/counting;
- Reset Communication and callback rebind;
- ordinary SDO health after GFC activity;
- restoration of the original test-modified parameter.

## Regression boundary

The process performs its own ordinary SDO smoke check. A claim that GFC does not disturb every other enabled CANopen protocol requires running the selected broader regression profile after GFC; it is not inferred from the GFC process alone.

This is protocol functional validation only. It does not establish SIL, PL, WCET/safety-time compliance, actuator-safe-state behavior, or system-level functional-safety certification.

## Detailed reference

The earlier detailed Chinese test note is retained at `../zh/reference/CANopen_GFC_Test.md`.
