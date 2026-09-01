[中文](../zh/srdo-test.md)

# SRDO protocol validation

The SRDO process validates the configured CANopenNode Safety-Related Data Object profile with protocol control through Lely/SDO and paired-frame wire evidence through the dedicated safety channel.

## Current default

`CANOPEN_ENABLE_SRDO_PROCESS=1` is the only automatic Master validation process enabled in the checked-in profile. Enabling it requires the MCU firmware and Object Dictionary to match the SRDO test profile expected by `src/srdo_process.cpp`.

## Coverage

The implementation covers the normal path and targeted fault cases, including:

- established-window baseline;
- NMT non-operational gating;
- valid receive pair and mapped-OD update;
- transmit pair and timing-window checks;
- Reset Communication, rebind, and natural periodic transmit behavior;
- ordinary PDO/SYNC/SDO/NMT regression checks relevant to the SRDO run;
- wrong inverse data;
- missing/incomplete pair timeouts;
- short frame and wrong frame order;
- checksum mismatch;
- configuration-validity invalidation;
- invalid COB-ID pair;
- invalid mapping count/length;
- TX inversion/silence behavior;
- final recovery verification.

## Safety statement

The test is a protocol implementation validation fixture. It does not certify the complete safety function, hardware redundancy, safe actuator state, diagnostic coverage, WCET, SIL, PL, or regulatory conformity.

## Shared wire channel

If GFC and SRDO are both enabled, they share the second Lely wire channel sequentially. SRDO drains stale wire evidence before using the channel so frames left by an earlier safety-protocol process are not accepted as SRDO proof.

## Detailed reference

The earlier detailed English note is retained at [`reference/CANopen_SRDO_Test.md`](reference/CANopen_SRDO_Test.md).
