/**
 * @file
 * @brief Implements HDR Filter Stage-1 validation for RT-Thread CAN HDR mode.
 */

#include "hdr_filter_process.h"

#include "canopen_config.h"
#include "canopen_master.h"
#include "canopen_nmt.h"
#include "canopen_sdo.h"
#include "nmt_heartbeat.h"

#include <lely/can/msg.h>
#include <lely/io2/linux/can.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <system_error>
#include <thread>

namespace {

constexpr std::uint16_t kHdrDiagnosticIndex = 0x2307U;
constexpr std::uint16_t kRpdo1CommIndex = 0x1400U;
constexpr std::uint8_t kRpdoCobIdSubindex = 0x01U;
constexpr std::uint16_t kRpdoValueIndex = 0x2200U;
constexpr std::uint8_t kScalarSubindex = 0x00U;
constexpr std::uint16_t kIdentityIndex = 0x1018U;
constexpr std::uint8_t kVendorIdSubindex = 0x01U;
constexpr std::uint16_t kHeartbeatConsumerIndex = 0x1016U;
constexpr std::uint8_t kHeartbeatConsumerSlot = 0x01U;
constexpr std::uint16_t kEmcyDiagnosticIndex = 0x2301U;
constexpr std::uint8_t kEmcyRxCountSubindex = 0x01U;

constexpr std::uint32_t kSyncCanId = 0x080U;
constexpr std::uint32_t kEmcyBaseCanId = 0x080U;
constexpr std::uint32_t kHeartbeatBaseCanId = 0x700U;
constexpr std::uint32_t kWrongFilterProbeCanId = 0x555U;

constexpr std::uint32_t kCobIdValidBit = 0x80000000UL;
constexpr std::uint32_t kStandardCanIdMask = 0x000007FFUL;

constexpr std::uint32_t kStateCanNormal = 1UL << 0;
constexpr std::uint32_t kStateHardwareFilters = 1UL << 1;
constexpr std::uint32_t kStateLimitOverride = 1UL << 2;

constexpr std::uint8_t kPathOptimized = 1U;
constexpr std::uint8_t kPathFallback = 2U;
constexpr std::uint8_t kPathFailed = 3U;

constexpr std::uint8_t kFaultOptimizedOnce = 1U;
constexpr std::uint8_t kFaultBothOnce = 3U;

constexpr std::uint32_t kRawStressFrames = 1000U;
constexpr std::uint32_t kResetStressCount = 20U;
constexpr int kWireWriteTimeoutMs = 500;
constexpr auto kNegativeObservation = std::chrono::milliseconds(80);
constexpr auto kRpdoUpdateTimeout = std::chrono::milliseconds(1200);
constexpr auto kPollPeriod = std::chrono::milliseconds(20);

struct HdrDiagnostic {
    std::uint32_t state_flags = 0U;
    std::uint16_t rx_size = 0U;
    std::uint16_t reported_maxhdr = 0U;
    std::uint16_t effective_maxhdr = 0U;
    std::uint32_t filter_generation = 0U;
    std::uint32_t optimized_attempts = 0U;
    std::uint32_t optimized_successes = 0U;
    std::uint32_t fallback_attempts = 0U;
    std::uint32_t fallback_successes = 0U;
    std::uint32_t fast_dispatch_count = 0U;
    std::uint32_t sw_dispatch_count = 0U;
    std::uint32_t hdr_invalid_count = 0U;
    std::uint32_t hdr_mismatch_count = 0U;
    std::uint32_t raw_dtr_rx_count = 0U;
    std::uint32_t raw_rtr_rx_count = 0U;
    std::int32_t last_filter_ret = 0;
    std::uint8_t fault_mode = 0U;
    std::uint32_t fault_request_seq = 0U;
    std::uint32_t fault_complete_seq = 0U;
    std::int32_t fault_result = 0;
    std::uint32_t stack_generation = 0U;
    std::uint8_t last_apply_path = 0U;
    std::uint16_t effective_maxhdr_override = 0U;
    std::uint32_t fail_closed_count = 0U;
};

template <class T>
bool readObject(CanopenTestMaster& master, std::uint16_t index,
                std::uint8_t subindex, T& value)
{
    return readRemoteSdo<T>(master, CANOPEN_SLAVE_NODE_ID, index, subindex,
                            value) == SdoOperationResult::SUCCESS;
}

template <class T>
bool writeObject(CanopenTestMaster& master, std::uint16_t index,
                 std::uint8_t subindex, T value)
{
    return writeRemoteSdo<T>(master, CANOPEN_SLAVE_NODE_ID, index, subindex,
                             value) == SdoOperationResult::SUCCESS;
}

bool readDiagnostic(CanopenTestMaster& master, HdrDiagnostic& diag)
{
#define READ_HDR_FIELD(type, sub, field) \
    if (!readObject<type>(master, kHdrDiagnosticIndex, sub, diag.field)) return false

    READ_HDR_FIELD(std::uint32_t, 0x01U, state_flags);
    READ_HDR_FIELD(std::uint16_t, 0x02U, rx_size);
    READ_HDR_FIELD(std::uint16_t, 0x03U, reported_maxhdr);
    READ_HDR_FIELD(std::uint16_t, 0x04U, effective_maxhdr);
    READ_HDR_FIELD(std::uint32_t, 0x05U, filter_generation);
    READ_HDR_FIELD(std::uint32_t, 0x06U, optimized_attempts);
    READ_HDR_FIELD(std::uint32_t, 0x07U, optimized_successes);
    READ_HDR_FIELD(std::uint32_t, 0x08U, fallback_attempts);
    READ_HDR_FIELD(std::uint32_t, 0x09U, fallback_successes);
    READ_HDR_FIELD(std::uint32_t, 0x0AU, fast_dispatch_count);
    READ_HDR_FIELD(std::uint32_t, 0x0BU, sw_dispatch_count);
    READ_HDR_FIELD(std::uint32_t, 0x0CU, hdr_invalid_count);
    READ_HDR_FIELD(std::uint32_t, 0x0DU, hdr_mismatch_count);
    READ_HDR_FIELD(std::uint32_t, 0x0EU, raw_dtr_rx_count);
    READ_HDR_FIELD(std::uint32_t, 0x0FU, raw_rtr_rx_count);
    READ_HDR_FIELD(std::int32_t, 0x10U, last_filter_ret);
    READ_HDR_FIELD(std::uint8_t, 0x11U, fault_mode);
    READ_HDR_FIELD(std::uint32_t, 0x12U, fault_request_seq);
    READ_HDR_FIELD(std::uint32_t, 0x13U, fault_complete_seq);
    READ_HDR_FIELD(std::int32_t, 0x14U, fault_result);
    READ_HDR_FIELD(std::uint32_t, 0x15U, stack_generation);
    READ_HDR_FIELD(std::uint8_t, 0x16U, last_apply_path);
    READ_HDR_FIELD(std::uint16_t, 0x17U, effective_maxhdr_override);
    READ_HDR_FIELD(std::uint32_t, 0x18U, fail_closed_count);

#undef READ_HDR_FIELD
    return true;
}

void logDiagnostic(const char* label, const HdrDiagnostic& diag)
{
    spdlog::info(
        "{}: state=0x{:08x} rx={} max={} eff={} gen={} path={} ret={} "
        "opt={}/{} fallback={}/{} fast={} sw={} invalid={} mismatch={} "
        "dtr={} rtr={} fault={}/{}/{} result={} stack={} limit={} failclosed={}",
        label, diag.state_flags, diag.rx_size, diag.reported_maxhdr,
        diag.effective_maxhdr, diag.filter_generation,
        static_cast<unsigned int>(diag.last_apply_path), diag.last_filter_ret,
        diag.optimized_successes, diag.optimized_attempts,
        diag.fallback_successes, diag.fallback_attempts,
        diag.fast_dispatch_count, diag.sw_dispatch_count,
        diag.hdr_invalid_count, diag.hdr_mismatch_count,
        diag.raw_dtr_rx_count, diag.raw_rtr_rx_count,
        static_cast<unsigned int>(diag.fault_mode), diag.fault_request_seq,
        diag.fault_complete_seq, diag.fault_result, diag.stack_generation,
        diag.effective_maxhdr_override, diag.fail_closed_count);
}

bool ensureOperational(CanopenTestMaster& master, const char* label)
{
    return issueNmtCommandAndWaitForState(
        master, lely::canopen::NmtCommand::START, CANOPEN_SLAVE_NODE_ID,
        lely::canopen::NmtState::START,
        std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS), label);
}

bool sendRawFrame(lely::io::CanChannel& channel, std::uint32_t can_id,
                  const std::uint8_t* data, std::uint8_t dlc, bool rtr = false)
{
    if (dlc > 8U || (!rtr && dlc != 0U && data == nullptr)) {
        return false;
    }

    can_msg message = CAN_MSG_INIT;
    message.id = can_id & kStandardCanIdMask;
    message.flags = rtr ? CAN_FLAG_RTR : 0U;
    message.len = rtr ? 0U : dlc;
    for (std::uint8_t i = 0U; i < message.len; ++i) {
        message.data[i] = data[i];
    }

    std::error_code error;
    channel.write(message, kWireWriteTimeoutMs, error);
    if (error) {
        spdlog::error("HDR raw CAN write failed: id=0x{:03x} rtr={}: {}",
                      message.id, rtr, error.message());
        return false;
    }
    return true;
}

bool sendRawRpdo(lely::io::CanChannel& channel, std::uint32_t can_id,
                 std::uint32_t value, bool rtr)
{
    const std::uint8_t data[4] = {
        static_cast<std::uint8_t>(value & 0xFFU),
        static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
        static_cast<std::uint8_t>((value >> 16U) & 0xFFU),
        static_cast<std::uint8_t>((value >> 24U) & 0xFFU),
    };
    return sendRawFrame(channel, can_id, data, 4U, rtr);
}

bool waitForRpdoValue(CanopenTestMaster& master, std::uint32_t expected)
{
    const auto deadline = std::chrono::steady_clock::now() + kRpdoUpdateTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::uint32_t value = 0U;
        if (readObject<std::uint32_t>(master, kRpdoValueIndex,
                                      kScalarSubindex, value)
            && value == expected) {
            return true;
        }
        std::this_thread::sleep_for(kPollPeriod);
    }
    spdlog::error("HDR RPDO value did not reach 0x{:08x}", expected);
    return false;
}

bool setRpdoBaseline(CanopenTestMaster& master, std::uint32_t value)
{
    return writeObject<std::uint32_t>(master, kRpdoValueIndex,
                                      kScalarSubindex, value);
}

bool writeRpdoCobId(CanopenTestMaster& master, std::uint32_t cob_id)
{
    return writeObject<std::uint32_t>(master, kRpdo1CommIndex,
                                      kRpdoCobIdSubindex, cob_id);
}

bool rebuildRpdoWithSameCobId(CanopenTestMaster& master,
                              std::uint32_t enabled_cob_id)
{
    const std::uint32_t disabled = enabled_cob_id | kCobIdValidBit;
    return writeRpdoCobId(master, disabled)
           && writeRpdoCobId(master, enabled_cob_id & ~kCobIdValidBit);
}

bool restoreRpdoCobId(CanopenTestMaster& master, std::uint32_t original)
{
    std::uint32_t current = 0U;
    if (!readObject<std::uint32_t>(master, kRpdo1CommIndex,
                                   kRpdoCobIdSubindex, current)) {
        return false;
    }

    if (!writeRpdoCobId(master, current | kCobIdValidBit)) {
        return false;
    }
    if (!writeRpdoCobId(master, original | kCobIdValidBit)) {
        return false;
    }
    return writeRpdoCobId(master, original & ~kCobIdValidBit);
}

bool armFault(CanopenTestMaster& master, std::uint8_t mode,
              std::uint32_t& sequence)
{
    HdrDiagnostic diag;
    if (!readDiagnostic(master, diag)) {
        return false;
    }
    sequence = diag.fault_request_seq + 1U;
    if (sequence == 0U) {
        sequence = 1U;
    }
    return writeObject<std::uint8_t>(master, kHdrDiagnosticIndex, 0x11U, mode)
           && writeObject<std::uint32_t>(master, kHdrDiagnosticIndex, 0x12U,
                                         sequence);
}

bool waitForEmcyRxCount(CanopenTestMaster& master, std::uint32_t minimum)
{
    const auto deadline = std::chrono::steady_clock::now() + kRpdoUpdateTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::uint32_t count = 0U;
        if (readObject<std::uint32_t>(master, kEmcyDiagnosticIndex,
                                      kEmcyRxCountSubindex, count)
            && count >= minimum) {
            return true;
        }
        std::this_thread::sleep_for(kPollPeriod);
    }
    return false;
}

bool runH00H01(CanopenTestMaster& master, lely::io::CanChannel& wire,
               std::uint32_t rpdo_cob_id)
{
    HdrDiagnostic before;
    if (!readDiagnostic(master, before)) {
        spdlog::error("H00: HDR diagnostic object 0x2307 unavailable");
        return false;
    }
    logDiagnostic("H00 baseline", before);
    if (before.rx_size == 0U || before.reported_maxhdr < 2U
        || before.last_apply_path != kPathOptimized
        || (before.state_flags & (kStateCanNormal | kStateHardwareFilters))
               != (kStateCanNormal | kStateHardwareFilters)) {
        spdlog::error("H00: optimized HDR baseline not established");
        return false;
    }
    if (before.hdr_invalid_count != 0U || before.hdr_mismatch_count != 0U) {
        spdlog::error("H00: HDR routing counters already report an error");
        return false;
    }

    /* SDO and NMT prove normal CANopen management traffic still reaches the
     * intended callbacks while optimized FilterMatchIndex dispatch is active. */
    std::uint32_t vendor_id = 0U;
    if (!readObject<std::uint32_t>(master, kIdentityIndex,
                                   kVendorIdSubindex, vendor_id)) {
        spdlog::error("H01: SDO identity read failed");
        return false;
    }
    if (!issueNmtCommandAndWaitForState(
            master, lely::canopen::NmtCommand::ENTER_PREOP,
            CANOPEN_SLAVE_NODE_ID, lely::canopen::NmtState::PREOP,
            std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS),
            "H01 enter pre-operational")) {
        return false;
    }
    if (!ensureOperational(master, "H01 return operational")) {
        return false;
    }

    /* Configure one otherwise-unused heartbeat-consumer slot for the Host
     * master, send one valid heartbeat, then restore the exact OD value. The
     * OD write itself also exercises runtime RX-filter reconstruction. */
    std::uint32_t heartbeat_original = 0U;
    if (!readObject<std::uint32_t>(master, kHeartbeatConsumerIndex,
                                   kHeartbeatConsumerSlot, heartbeat_original)) {
        spdlog::error("H01: heartbeat-consumer baseline read failed");
        return false;
    }
    const std::uint32_t heartbeat_profile =
        (static_cast<std::uint32_t>(CANOPEN_MASTER_NODE_ID) << 16U) | 1000U;
    if (!writeObject<std::uint32_t>(master, kHeartbeatConsumerIndex,
                                    kHeartbeatConsumerSlot, heartbeat_profile)) {
        spdlog::error("H01: heartbeat-consumer test configuration failed");
        return false;
    }
    const std::uint8_t heartbeat_state = 0x05U;
    if (!sendRawFrame(wire, kHeartbeatBaseCanId + CANOPEN_MASTER_NODE_ID,
                      &heartbeat_state, 1U)) {
        (void)writeObject<std::uint32_t>(master, kHeartbeatConsumerIndex,
                                         kHeartbeatConsumerSlot,
                                         heartbeat_original);
        return false;
    }
    std::this_thread::sleep_for(kNegativeObservation);
    if (!writeObject<std::uint32_t>(master, kHeartbeatConsumerIndex,
                                    kHeartbeatConsumerSlot,
                                    heartbeat_original)) {
        spdlog::error("H01: heartbeat-consumer configuration restore failed");
        return false;
    }

    /* SYNC has no application diagnostic in the demo OD. Its correct fast-path
     * selection is therefore proven by the hdr_index mismatch observer below. */
    if (!sendRawFrame(wire, kSyncCanId, nullptr, 0U)) {
        spdlog::error("H01: SYNC probe send failed");
        return false;
    }

    /* The demo EMCY-consumer diagnostic gives semantic evidence that a remote
     * EMCY traversed the selected receive buffer and reached its callback. */
    std::uint32_t emcy_before = 0U;
    if (!readObject<std::uint32_t>(master, kEmcyDiagnosticIndex,
                                   kEmcyRxCountSubindex, emcy_before)) {
        spdlog::error("H01: EMCY consumer diagnostic is unavailable");
        return false;
    }
    const std::uint8_t emcy_data[8] = {0x00U, 0x10U, 0x01U, 0x00U,
                                       0x48U, 0x44U, 0x52U, 0x01U};
    if (!sendRawFrame(wire, kEmcyBaseCanId + CANOPEN_MASTER_NODE_ID,
                      emcy_data, static_cast<std::uint8_t>(sizeof(emcy_data)))
        || !waitForEmcyRxCount(master, emcy_before + 1U)) {
        spdlog::error("H01: EMCY consumer did not observe the raw probe");
        return false;
    }

    /* RPDO delivery provides a data-path assertion rather than a counter-only
     * observation. */
    const std::uint32_t probe = 0x13572468UL;
    if (!setRpdoBaseline(master, ~probe)
        || !sendRawRpdo(wire, rpdo_cob_id, probe, false)
        || !waitForRpdoValue(master, probe)) {
        spdlog::error("H01: RPDO delivery failed on optimized filters");
        return false;
    }

    HdrDiagnostic after;
    if (!readDiagnostic(master, after)) {
        return false;
    }
    if (after.hdr_invalid_count != 0U || after.hdr_mismatch_count != 0U
        || after.sw_dispatch_count != before.sw_dispatch_count
        || after.fast_dispatch_count <= before.fast_dispatch_count
        || after.filter_generation <= before.filter_generation) {
        spdlog::error("H01: optimized NMT/HB/SDO/PDO/SYNC/EMCY dispatch evidence is incomplete");
        return false;
    }
    spdlog::info("H00/H01 PASS: optimized HDR filters preserve NMT/HB/SDO/PDO/SYNC/EMCY paths; vendor=0x{:08x}", vendor_id);
    return true;
}

bool runH02(CanopenTestMaster& master, lely::io::CanChannel& wire,
            std::uint32_t rpdo_cob_id)
{
    HdrDiagnostic before;
    if (!readDiagnostic(master, before)) {
        return false;
    }

    std::uint32_t last_valid = 0U;
    const std::uint32_t wrong_id = kWrongFilterProbeCanId;
    for (std::uint32_t i = 0U; i < kRawStressFrames; ++i) {
        if ((i % 3U) == 0U) {
            last_valid = 0xA5000000UL | i;
            if (!sendRawRpdo(wire, rpdo_cob_id, last_valid, false)) {
                return false;
            }
        } else if ((i % 3U) == 1U) {
            if (!sendRawRpdo(wire, wrong_id, 0x5A000000UL | i, false)) {
                return false;
            }
        } else if (!sendRawRpdo(wire, rpdo_cob_id, 0U, true)) {
            return false;
        }
    }

    if (!waitForRpdoValue(master, last_valid)) {
        return false;
    }

    HdrDiagnostic after;
    if (!readDiagnostic(master, after)) {
        return false;
    }
    if (after.hdr_invalid_count != before.hdr_invalid_count
        || after.hdr_mismatch_count != before.hdr_mismatch_count
        || after.sw_dispatch_count != before.sw_dispatch_count
        || after.raw_rtr_rx_count != before.raw_rtr_rx_count) {
        spdlog::error("H02: hdr_index routing was not stable under mixed traffic");
        return false;
    }
    spdlog::info("H02 PASS: {} mixed frames kept hdr_index routing stable", kRawStressFrames);
    return true;
}

bool runH03(CanopenTestMaster& master, lely::io::CanChannel& wire,
            std::uint32_t original_cob_id)
{
    HdrDiagnostic before;
    if (!readDiagnostic(master, before)) {
        return false;
    }

    const std::uint32_t old_id = original_cob_id & kStandardCanIdMask;
    std::uint32_t new_id = (old_id + 0x20U) & kStandardCanIdMask;
    if (new_id == 0U || new_id == old_id) {
        new_id = (old_id ^ 0x20U) & kStandardCanIdMask;
    }
    const std::uint32_t alt_cob_id =
        (original_cob_id & ~kStandardCanIdMask) | new_id;

    if (!writeRpdoCobId(master, original_cob_id | kCobIdValidBit)
        || !writeRpdoCobId(master, alt_cob_id | kCobIdValidBit)
        || !writeRpdoCobId(master, alt_cob_id & ~kCobIdValidBit)) {
        spdlog::error("H03: standard RPDO disable/change/enable sequence failed");
        (void)restoreRpdoCobId(master, original_cob_id);
        return false;
    }

    const std::uint32_t baseline = 0x11223344UL;
    const std::uint32_t wrong = 0x55667788UL;
    const std::uint32_t valid = 0x99AABBCCUL;
    if (!setRpdoBaseline(master, baseline)
        || !sendRawRpdo(wire, old_id, wrong, false)) {
        (void)restoreRpdoCobId(master, original_cob_id);
        return false;
    }
    std::this_thread::sleep_for(kNegativeObservation);
    std::uint32_t observed = 0U;
    if (!readObject<std::uint32_t>(master, kRpdoValueIndex,
                                   kScalarSubindex, observed)
        || observed != baseline) {
        spdlog::error("H03: old RPDO CAN-ID remained active after rebuild");
        (void)restoreRpdoCobId(master, original_cob_id);
        return false;
    }
    if (!sendRawRpdo(wire, new_id, valid, false)
        || !waitForRpdoValue(master, valid)) {
        spdlog::error("H03: new RPDO CAN-ID was not activated");
        (void)restoreRpdoCobId(master, original_cob_id);
        return false;
    }

    if (!restoreRpdoCobId(master, original_cob_id)) {
        spdlog::error("H03: failed to restore original RPDO COB-ID");
        return false;
    }

    HdrDiagnostic after;
    if (!readDiagnostic(master, after)) {
        return false;
    }
    if (after.filter_generation <= before.filter_generation
        || after.last_apply_path != kPathOptimized
        || after.hdr_invalid_count != before.hdr_invalid_count
        || after.hdr_mismatch_count != before.hdr_mismatch_count) {
        spdlog::error("H03: dynamic filter reconstruction evidence is invalid");
        return false;
    }
    spdlog::info("H03 PASS: RPDO COB-ID rebuilt 0x{:03x}->0x{:03x}->0x{:03x}",
                 old_id, new_id, old_id);
    return true;
}

bool runH05(CanopenTestMaster& master, std::uint32_t rpdo_cob_id)
{
    HdrDiagnostic before;
    if (!readDiagnostic(master, before)) {
        return false;
    }

    std::uint32_t sequence = 0U;
    if (!armFault(master, kFaultOptimizedOnce, sequence)) {
        return false;
    }
    if (!writeRpdoCobId(master, rpdo_cob_id | kCobIdValidBit)) {
        spdlog::error("H05: disabling RPDO after fault arm failed");
        return false;
    }

    HdrDiagnostic fallback;
    if (!readDiagnostic(master, fallback)) {
        return false;
    }
    if (fallback.fault_complete_seq != sequence || fallback.fault_result != 0
        || fallback.fallback_successes <= before.fallback_successes
        || fallback.last_apply_path != kPathFallback
        || (fallback.state_flags & kStateCanNormal) == 0U
        || (fallback.state_flags & kStateHardwareFilters) != 0U) {
        spdlog::error("H05: optimized failure did not enter healthy fallback");
        return false;
    }

    if (!writeRpdoCobId(master, rpdo_cob_id & ~kCobIdValidBit)) {
        return false;
    }
    HdrDiagnostic recovered;
    if (!readDiagnostic(master, recovered)
        || recovered.last_apply_path != kPathOptimized
        || (recovered.state_flags & kStateHardwareFilters) == 0U) {
        spdlog::error("H05: optimized filter path did not recover after one-shot fault");
        return false;
    }
    spdlog::info("H05 PASS: optimized failure fell back safely and recovered");
    return true;
}

bool runH04H07(CanopenTestMaster& master, lely::io::CanChannel& wire,
               std::uint32_t rpdo_cob_id)
{
    HdrDiagnostic before;
    if (!readDiagnostic(master, before) || before.rx_size < 2U
        || before.reported_maxhdr < 2U) {
        spdlog::error("H04: insufficient diagnostic/HDR capacity for fallback test");
        return false;
    }

    const std::uint16_t injected_limit =
        static_cast<std::uint16_t>(before.rx_size - 1U);
    if (!writeObject<std::uint16_t>(master, kHdrDiagnosticIndex, 0x17U,
                                    injected_limit)
        || !rebuildRpdoWithSameCobId(master, rpdo_cob_id)) {
        spdlog::error("H04: failed to inject effective HDR shortage");
        return false;
    }

    HdrDiagnostic fallback;
    if (!readDiagnostic(master, fallback)) {
        return false;
    }
    if (fallback.effective_maxhdr >= fallback.rx_size
        || fallback.last_apply_path != kPathFallback
        || (fallback.state_flags & kStateCanNormal) == 0U
        || (fallback.state_flags & kStateHardwareFilters) != 0U
        || (fallback.state_flags & kStateLimitOverride) == 0U) {
        spdlog::error("H04: injected HDR shortage did not select software fallback");
        return false;
    }
    spdlog::info("H04 PASS: injected effective maxhdr {} < rxSize {} selected fallback",
                 fallback.effective_maxhdr, fallback.rx_size);

    const std::uint32_t baseline = 0xCAFEBABEU;
    const std::uint32_t valid = 0x0BADF00DU;
    if (!setRpdoBaseline(master, baseline)) {
        return false;
    }
    HdrDiagnostic before_rtr;
    if (!readDiagnostic(master, before_rtr)
        || !sendRawRpdo(wire, rpdo_cob_id, 0U, true)) {
        return false;
    }
    std::this_thread::sleep_for(kNegativeObservation);
    std::uint32_t observed = 0U;
    HdrDiagnostic after_rtr;
    if (!readObject<std::uint32_t>(master, kRpdoValueIndex,
                                   kScalarSubindex, observed)
        || !readDiagnostic(master, after_rtr)
        || observed != baseline
        || after_rtr.raw_rtr_rx_count <= before_rtr.raw_rtr_rx_count
        || after_rtr.sw_dispatch_count <= before_rtr.sw_dispatch_count) {
        spdlog::error("H07: fallback RTR acceptance/software rejection contract failed");
        return false;
    }
    if (!sendRawRpdo(wire, rpdo_cob_id, valid, false)
        || !waitForRpdoValue(master, valid)) {
        spdlog::error("H07: fallback DTR delivery failed");
        return false;
    }
    spdlog::info("H07 PASS: fallback accepts DTR/RTR and software matcher rejects RPDO RTR");

    if (!writeObject<std::uint16_t>(master, kHdrDiagnosticIndex, 0x17U, 0U)
        || !rebuildRpdoWithSameCobId(master, rpdo_cob_id)) {
        spdlog::error("H04/H07: failed to clear injected HDR shortage");
        return false;
    }
    HdrDiagnostic recovered;
    if (!readDiagnostic(master, recovered)
        || recovered.last_apply_path != kPathOptimized
        || (recovered.state_flags & kStateHardwareFilters) == 0U) {
        spdlog::error("H04/H07: optimized path did not recover after clearing limit");
        return false;
    }
    return true;
}

bool resetCommunicationAndWait(CanopenTestMaster& master, const char* label)
{
    prepareBootWait();
    if (!issueNmtCommand(master, lely::canopen::NmtCommand::RESET_COMM,
                         CANOPEN_SLAVE_NODE_ID, label)) {
        return false;
    }
    if (!waitForBootCompletion(std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS))) {
        spdlog::error("{}: Boot callback did not arrive", label);
        return false;
    }
    return ensureOperational(master, label);
}

bool runH06B(CanopenTestMaster& master, std::uint32_t original_cob_id)
{
    HdrDiagnostic before;
    if (!readDiagnostic(master, before)) {
        return false;
    }
    std::uint32_t sequence = 0U;
    if (!armFault(master, kFaultBothOnce, sequence)) {
        return false;
    }

    const SdoOperationResult write_result = writeRemoteSdo<std::uint32_t>(
        master, CANOPEN_SLAVE_NODE_ID, kRpdo1CommIndex, kRpdoCobIdSubindex,
        original_cob_id | kCobIdValidBit);
    if (write_result == SdoOperationResult::SUCCESS) {
        spdlog::warn("H06-B: RPDO disable SDO completed despite injected filter failure; checking fail-closed diagnostics");
    }

    HdrDiagnostic failed;
    const bool immediate_diag = readDiagnostic(master, failed);
    if (immediate_diag) {
        if (failed.fault_complete_seq != sequence || failed.fault_result == 0
            || failed.fail_closed_count <= before.fail_closed_count
            || failed.last_apply_path != kPathFailed
            || (failed.state_flags & kStateCanNormal) != 0U) {
            spdlog::error("H06-B: double failure did not publish fail-closed state");
            return false;
        }
    } else {
        spdlog::warn("H06-B: SDO diagnostics unavailable after fail-closed; use MCU 'canopen_filter_diag' for out-of-band evidence");
    }

    if (!resetCommunicationAndWait(master, "H06-B recovery reset communication")) {
        spdlog::error("H06-B: one-shot fault did not permit communication-reset recovery");
        return false;
    }
    if (!restoreRpdoCobId(master, original_cob_id)) {
        spdlog::error("H06-B: failed to restore RPDO COB-ID after recovery");
        return false;
    }

    HdrDiagnostic recovered;
    if (!readDiagnostic(master, recovered)
        || recovered.fault_complete_seq != sequence
        || recovered.fault_result == 0
        || recovered.fail_closed_count <= before.fail_closed_count
        || recovered.stack_generation <= before.stack_generation
        || recovered.last_apply_path != kPathOptimized
        || (recovered.state_flags & (kStateCanNormal | kStateHardwareFilters))
               != (kStateCanNormal | kStateHardwareFilters)) {
        spdlog::error("H06-B: recovery evidence is incomplete");
        return false;
    }
    spdlog::info("H06-B PASS: optimized+fallback failure forced fail-closed and recovered after one-shot reset");
    return true;
}

bool runH08(CanopenTestMaster& master)
{
    HdrDiagnostic before;
    if (!readDiagnostic(master, before)) {
        return false;
    }
    for (std::uint32_t i = 0U; i < kResetStressCount; ++i) {
        if (!resetCommunicationAndWait(master, "H08 Reset Communication stress")) {
            spdlog::error("H08: reset iteration {} failed", i + 1U);
            return false;
        }
    }

    HdrDiagnostic after;
    if (!readDiagnostic(master, after)) {
        return false;
    }
    if (after.stack_generation < before.stack_generation + kResetStressCount
        || after.filter_generation < before.filter_generation + kResetStressCount
        || after.last_apply_path != kPathOptimized
        || after.hdr_invalid_count != before.hdr_invalid_count
        || after.hdr_mismatch_count != before.hdr_mismatch_count) {
        spdlog::error("H08: filter reconstruction/reset-generation evidence is invalid");
        return false;
    }
    spdlog::info("H08 PASS: {} Reset Communication cycles rebuilt HDR filters cleanly",
                 kResetStressCount);
    return true;
}

#if CANOPEN_HDR_FILTER_RUN_DESTRUCTIVE_RESET_FAIL_TEST
bool runH06A(CanopenTestMaster& master)
{
    std::uint32_t sequence = 0U;
    if (!armFault(master, kFaultBothOnce, sequence)) {
        return false;
    }
    prepareBootWait();
    if (!issueNmtCommand(master, lely::canopen::NmtCommand::RESET_COMM,
                         CANOPEN_SLAVE_NODE_ID,
                         "H06-A destructive reset communication")) {
        return false;
    }
    if (waitForBootCompletion(std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS))) {
        spdlog::error("H06-A: DUT unexpectedly re-entered CAN normal mode after double filter failure");
        return false;
    }
    spdlog::info("H06-A PASS: no Boot after reset-entry double filter failure; hardware reset is now required");
    return true;
}
#endif

} // namespace

int hdrFilterProcess(CanopenTestMaster& master,
                     lely::io::CanChannel& wire_channel)
{
    if (!ensureOperational(master, "HDR Stage-1 initial operational state")) {
        return 1;
    }

    std::uint32_t original_cob_id = 0U;
    if (!readObject<std::uint32_t>(master, kRpdo1CommIndex,
                                   kRpdoCobIdSubindex, original_cob_id)) {
        return 1;
    }
    if ((original_cob_id & kCobIdValidBit) != 0U) {
        spdlog::error("HDR Stage-1 requires RPDO1 enabled at startup");
        return 1;
    }
    const std::uint32_t rpdo_can_id = original_cob_id & kStandardCanIdMask;

    bool ok = runH00H01(master, wire_channel, rpdo_can_id);
    if (ok) ok = runH02(master, wire_channel, rpdo_can_id);
    if (ok) ok = runH03(master, wire_channel, original_cob_id);
    if (ok) ok = runH05(master, original_cob_id);
    if (ok) ok = runH04H07(master, wire_channel, original_cob_id);
    if (ok) ok = runH06B(master, original_cob_id);
    if (ok) ok = runH08(master);

#if CANOPEN_HDR_FILTER_RUN_DESTRUCTIVE_RESET_FAIL_TEST
    if (ok) ok = runH06A(master);
#else
    spdlog::info("H06-A destructive reset-entry failure case is compiled out; enable CANOPEN_HDR_FILTER_RUN_DESTRUCTIVE_RESET_FAIL_TEST for a dedicated final run");
#endif

    if (ok) {
        spdlog::info("H09 conditional LSS bitrate test remains target/HIL-only; verify filter_generation across the existing 1000->500->1000 kbit/s LSS procedure");
    }

    if (!CANOPEN_HDR_FILTER_RUN_DESTRUCTIVE_RESET_FAIL_TEST) {
        /* Best-effort cleanup keeps normal regression runs reusable. */
        (void)writeObject<std::uint16_t>(master, kHdrDiagnosticIndex, 0x17U, 0U);
        (void)restoreRpdoCobId(master, original_cob_id);
        (void)ensureOperational(master, "HDR Stage-1 cleanup operational state");
    }

    return ok ? 0 : 1;
}
