# J09 / B09S SRDO protocol validation

This stage validates the CANopenNode CiA 304 SRDO protocol implementation on the
selected STM32F407/RT-Thread target configuration. It does **not** claim
functional-safety certification, SIL/PL compliance, FMEDA coverage, redundant
hardware validation, safe-actuator-chain validation, or machine hazard analysis.

## Host/fixture split

The existing Lely `AsyncMaster` remains responsible for SDO, NMT and Boot
control. `main.cpp` creates one secondary `safety_wire_channel` on the same
SocketCAN interface and passes it to the enabled B09G GFC and J09/B09S SRDO
processes in the ordered process table. This channel is a **sequential test
resource**, not a concurrent multiplexer: only the currently running safety
process may consume it. J09/B09S drains the channel before its first capture so
frames left by an earlier safety stage cannot become SRDO evidence. The fixture
intentionally exposes no arbitrary CAN transmit API.

| Direction | Normal | Inverted |
|---|---:|---:|
| Host -> MCU, SRDO1 RX | `0x101` | `0x102` |
| MCU -> Host, SRDO2 TX | `0x103` | `0x104` |

The baseline application payload is `0x12345678`; the inverted payload is
`0xEDCBA987`. The Host sends the RX normal frame and then the inverted frame with
a 5 ms controlled delay. TX capture propagates the receive timestamp returned
by Lely `CanChannel::read()` for each SocketCAN frame.

TX pair acquisition uses a stateful `EXPECT_NORMAL` / `EXPECT_INVERTED`
synchronizer. After a normal frame is accepted, only its inverted member may
complete that pair; duplicate-normal and inverted-before-normal ordering are
reported as errors, and the inverted member has a bounded pair deadline. For
natural cyclic sampling only, capture may discard one leading inverted frame
while finding the first normal-frame anchor because observation can begin in the
middle of an already-running pair. A second inverted-before-normal frame is an
ordering failure. After the anchor, the synchronizer remains strict and cannot
silently pair frames across cycles.

Triggered TX requests treat the pinned CANopenNode `CO_ERROR_TX_BUSY` result
(`-15`) as a legal transient only. The Host allows at most four retries. Before
each retry it refreshes the RX keepalive and consumes the pending natural TX
pair through its inverted member within the bounded pair deadline, so the next
request is issued only after the TX phase is requestable again. Every retry uses
a new request sequence and logs the retry count; the final request result is
always logged. No other non-zero request result is retried, so a persistent busy
or any other SRDO request error remains a functional test failure rather than
being hidden by the harness.

## Required target OD contract

The target must expose the standard SRDO objects `0x1301`, `0x1302`, `0x1381`,
`0x1382`, `0x13FE` and `0x13FF`, plus test-only record `0x2306`:

| Sub-index | Type | Meaning |
|---:|---|---|
| `01` | U32 | SRDO1 RX normal mapped value |
| `02` | U32 | SRDO1 RX inverted mapped value |
| `03` | U32 | SRDO2 TX normal mapped value |
| `04` | U32 | SRDO2 TX inverted mapped value |
| `05` | I8 | aggregate `CO_process_SRDO()` state |
| `06` | I8 | SRDO1 RX state |
| `07` | I8 | SRDO2 TX state |
| `08` | U32 | state snapshot sequence |
| `09` | U32 | Host TX request sequence |
| `0A` | U32 | MCU TX completion sequence |
| `0B` | I32 | request result / reset-generation witness (`INT32_MIN` = no diagnostic request processed) |

The expected baseline is SCT 100 ms, SRVT 20 ms, transmission type 254, two
mapping entries per SRDO, RX mapping `0x23060120/0x23060220`, TX mapping
`0x23060320/0x23060420`, and configuration-valid value `0xA5`. The Host
recomputes the CANopenNode CRC16-CCITT signature instead of accepting a hard
coded checksum.

## Automated coverage

Functional coverage includes Pre-operational state reporting, exact profile and
CRC validation, RX normal/inverted pair processing, mapped RX values, TX pair
capture, and aggregate state establishment held while valid RX traffic covers
more than one complete 100 ms SCT window. Established/recovery windows start
before unrelated TX-control work, and lightweight state polling reads only the
required diagnostic sub-indexes so Host SDO scheduling does not consume the RX
SCT budget. Timing coverage collects 200 natural TX pairs (199 cycle intervals),
and PDO/SYNC/SDO/NMT regression runs with an RAII-managed RX keepalive.

Reset Communication validation does not require a post-reset bus-silence window.
Transmission type 254 may legitimately resume natural cyclic SRDO TX after the
node returns Operational. The test therefore accepts and validates several natural periodic TX pairs while
keeping valid RX traffic active. It requires `tx_request_seq` and
`tx_complete_seq` to remain equal to their pre-reset values and requires
`tx_request_result == INT32_MIN` throughout the natural-TX observation. The MCU
sets that sentinel on the newly bound SRDO generation and changes it only when a
diagnostic request is actually processed, separating natural production from
replay of a stale request.

Fault coverage includes wrong RX inverse, missing inverted frame (SRVT timeout),
missing complete pair (SCT timeout), short DLC, inverted-first and duplicate
normal ordering faults, wrong 0x13FF checksum, 0x13FE invalidation after a legal
configuration write, illegal and non-consecutive COB-ID cases, odd mapping-count
rejection, normal/inverted mapping-length mismatch, TX data not inverted, and a
final reset/recovery validation. Negative SRDO states are treated as latched;
the test repairs the configuration and uses Reset Communication before the next
runtime fault case.

Expected SDO rejection tests accept only errors from Lely's SDO-abort category
and require the case-specific abort code: `PARAM_VAL` for the invalid F08 COB-ID
writes and `PDO_LEN` for the F09 odd mapping count. Local
socket/executor/system errors and SDO timeouts are failures, not evidence that
the target correctly rejected an OD write. State/TX-completion polling uses one
absolute `steady_clock` deadline and reads only the required diagnostic fields;
all reads consume the remaining budget instead of restarting a full timeout.

## Timing boundary

The target realtime request is 1000 us and `CO_CONFIG_SRDO_MINIMUM_DELAY` is
initially 0. The test records observable pair/cycle min/max/average values from
the SocketCAN receive timestamps. It requires at least 200 TX pairs and 100
cycle intervals; the current run collects 200 pairs and 199 intervals.

For protocol/profile checking, normal-to-inverted receive-observation delay must
remain within a deliberately tolerant 30 ms limit around the configured 20 ms
SRVT, individual natural normal-to-normal cycles must remain within 50..150 ms,
and the average cycle must remain within 80..120 ms around the configured 100 ms
SCT. These are Host-observation tolerances for protocol testing, not a safety
timing budget, WCET result, SIL/PL result, or sub-millisecond guarantee.

The only accepted final PASS wording is:

`CANopenNode GFC/SRDO protocol functional verification PASS on the tested STM32F407/RT-Thread configuration.`