/**
 * @file
 * @brief Implements B06 EMCY consumer validation.
 */

#include "emcy_consumer_process.h"

#include "canopen_config.h"
#include "canopen_master.h"
#include "canopen_nmt.h"
#include "canopen_sdo.h"
#include "nmt_heartbeat.h"

#include <lely/coapp/master.hpp>

#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <system_error>
#include <thread>

namespace {

/** MCU demo diagnostic record for received remote EMCY messages. */
constexpr std::uint16_t kDiagnosticIndex = 0x2301;
/** Remote EMCY callback count. */
constexpr std::uint8_t kDiagnosticCountSubindex = 0x01;
/** Last remote EMCY source Node-ID. */
constexpr std::uint8_t kDiagnosticSourceNodeSubindex = 0x02;
/** Last remote EMCY CAN-ID/COB-ID. */
constexpr std::uint8_t kDiagnosticCobIdSubindex = 0x03;
/** Last remote EMCY error code. */
constexpr std::uint8_t kDiagnosticErrorCodeSubindex = 0x04;
/** Last remote EMCY Error Register. */
constexpr std::uint8_t kDiagnosticErrorRegisterSubindex = 0x05;
/** Last CANopenNode EMCY callback errorBit. */
constexpr std::uint8_t kDiagnosticErrorBitSubindex = 0x06;
/** Last CANopenNode EMCY callback infoCode. */
constexpr std::uint8_t kDiagnosticInfoCodeSubindex = 0x07;

/** Host-local Pre-defined Error Field. */
constexpr std::uint16_t kLocalErrorHistoryIndex = 0x1003;
/** Host-local EMCY producer COB-ID object. */
constexpr std::uint16_t kLocalEmcyCobIdIndex = 0x1014;
/** Host-local EMCY inhibit-time object. */
constexpr std::uint16_t kLocalEmcyInhibitIndex = 0x1015;
/** Scalar sub-index used by local communication parameters. */
constexpr std::uint8_t kScalarSubindex = 0x00;
/** CANopen invalid bit used by 0x1014. */
constexpr std::uint32_t kCobIdInvalidMask = 0x80000000U;
/** Expected CiA 301 EMCY CAN-ID for the configured Host master node. */
constexpr std::uint32_t kExpectedHostEmcyCanId = 0x80U + CANOPEN_MASTER_NODE_ID;

/** B06 SDO transaction timeout; diagnostic reads should fail fast. */
constexpr std::uint32_t kSdoTimeoutMs = 500U;
/** Local completion margin after one B06 SDO timeout. */
constexpr std::uint32_t kSdoCompletionMarginMs = 100U;
/** Maximum wait for one newly published MCU EMCY diagnostic. */
constexpr std::uint32_t kEmcyObservationTimeoutMs = 2000U;
/** Delay between remote diagnostic count polls. */
constexpr std::uint32_t kDiagnosticPollIntervalMs = 20U;
/** Maximum retries when a multi-SDO diagnostic snapshot changes mid-read. */
constexpr unsigned int kSnapshotRetryCount = 3U;

/** Manufacturer/device-specific error code used by vector A. */
constexpr std::uint16_t kErrorCodeA = 0xFF01U;
/** Manufacturer/device-specific error code used by vector B. */
constexpr std::uint16_t kErrorCodeB = 0xFF02U;
/** Explicit Error Register including generic and manufacturer-specific bits. */
constexpr std::uint8_t kErrorRegister = 0x81U;

/** Expected infoCode from vector A bytes 12 34 56 78. */
constexpr std::uint32_t kInfoCodeA = 0x78563412U;
/** Expected infoCode from vector B bytes 87 65 43 21. */
constexpr std::uint32_t kInfoCodeB = 0x21436587U;

/** One deterministic Host-generated EMCY test vector. */
struct EmcyTestVector {
    std::uint16_t error_code;
    std::uint8_t error_register;
    std::array<std::uint8_t, 5U> msef;
    std::uint8_t expected_error_bit;
    std::uint32_t expected_info_code;
    const char* name;
};

const EmcyTestVector kVectorA = {
    kErrorCodeA, kErrorRegister, {{0xA1U, 0x12U, 0x34U, 0x56U, 0x78U}},
    0xA1U, kInfoCodeA, "A"};
const EmcyTestVector kVectorB = {
    kErrorCodeB, kErrorRegister, {{0xB2U, 0x87U, 0x65U, 0x43U, 0x21U}},
    0xB2U, kInfoCodeB, "B"};

/** Coherent MCU 0x2301 diagnostic snapshot. */
struct EmcyConsumerSnapshot {
    std::uint32_t rx_count = 0;
    std::uint8_t source_node_id = 0;
    std::uint16_t cob_id = 0;
    std::uint16_t error_code = 0;
    std::uint8_t error_register = 0;
    std::uint8_t error_bit = 0;
    std::uint32_t info_code = 0;
};

/** Runtime state required for cleanup after an interrupted B06 process. */
struct EmcyConsumerRuntimeState {
    bool host_error_active = false;
    unsigned int expected_host_error_count = 0U;
    bool reset_communication_issued = false;
    bool remote_operational_restored = false;
};

/**
 * @brief Read one Host-local OD value.
 */
template <class T>
bool readLocalObject(lely::canopen::AsyncMaster& master, std::uint16_t index,
                     std::uint8_t subindex, T& value, const char* label)
{
    std::error_code error;
    const T read_value = master.Read<T>(index, subindex, error);
    if (error) {
        spdlog::error("B06 unable to read local {} (0x{:04x}:{:02x}): {}",
                      label, index, static_cast<unsigned int>(subindex),
                      error.message());
        return false;
    }
    value = read_value;
    return true;
}

/**
 * @brief Read one MCU diagnostic value with the B06 fail-fast SDO budget.
 */
template <class T>
bool readRemoteDiagnostic(lely::canopen::AsyncMaster& master,
                          std::uint16_t index, std::uint8_t subindex,
                          T& value, const char* label)
{
    if (readRemoteSdo<T>(
            master, CANOPEN_SLAVE_NODE_ID, index, subindex, value,
            std::chrono::milliseconds(kSdoTimeoutMs),
            std::chrono::milliseconds(kSdoCompletionMarginMs))
        != SdoOperationResult::SUCCESS) {
        spdlog::error("B06 remote diagnostic read failed: {}", label);
        return false;
    }
    return true;
}

/**
 * @brief Check the Host EMCY producer configuration and require no active local EMCY.
 */
bool validateProducerPreflight(EmcyTestMaster& master)
{
    std::uint32_t cob_id = 0;
    std::uint16_t inhibit_time = 0;
    if (!readLocalObject(master, kLocalEmcyCobIdIndex, kScalarSubindex, cob_id,
                         "COB-ID EMCY")
        || !readLocalObject(master, kLocalEmcyInhibitIndex, kScalarSubindex,
                            inhibit_time, "Inhibit time EMCY")) {
        return false;
    }

    if ((cob_id & kCobIdInvalidMask) != 0U) {
        spdlog::error("B06 Host EMCY producer is disabled: 0x1014=0x{:08x}",
                      cob_id);
        return false;
    }
    if (cob_id != kExpectedHostEmcyCanId) {
        spdlog::error(
            "B06 Host EMCY COB-ID mismatch: expected=0x{:08x} actual=0x{:08x}",
            kExpectedHostEmcyCanId, cob_id);
        return false;
    }
    if (inhibit_time != 0U) {
        spdlog::error(
            "B06 requires Host 0x1015=0 for deterministic EMCY timing: actual={}",
            static_cast<unsigned int>(inhibit_time));
        return false;
    }

    /* Do not use 0x1003:00 as the initial stack-depth check. Lely initializes
     * its EMCY service with an empty internal stack but does not immediately
     * synchronize a DCF-backed 0x1003 until the stack is first modified. */
    std::uint16_t active_error = 0;
    std::uint8_t active_register = 0;
    if (!master.peekLocalEmcy(active_error, active_register)) {
        spdlog::error("B06 Host Lely EMCY service is unavailable");
        return false;
    }
    if (active_error != 0U || active_register != 0U) {
        spdlog::error(
            "B06 refuses to clear a pre-existing Host EMCY: code=0x{:04x} register=0x{:02x}",
            static_cast<unsigned int>(active_error),
            static_cast<unsigned int>(active_register));
        return false;
    }

    spdlog::info(
        "B06 Host EMCY producer preflight passed: node={} CAN-ID=0x{:03x} inhibit=0 active_stack=empty",
        CANOPEN_MASTER_NODE_ID, kExpectedHostEmcyCanId);
    return true;
}

/**
 * @brief Read one coherent MCU 0x2301 snapshot across independent SDO uploads.
 */
bool readStableSnapshot(lely::canopen::AsyncMaster& master,
                        EmcyConsumerSnapshot& snapshot)
{
    for (unsigned int attempt = 0; attempt < kSnapshotRetryCount; ++attempt) {
        std::uint32_t count_before = 0;
        std::uint32_t count_after = 0;
        EmcyConsumerSnapshot current;

        if (!readRemoteDiagnostic(master, kDiagnosticIndex,
                                  kDiagnosticCountSubindex, count_before,
                                  "0x2301 remote_rx_count before")
            || !readRemoteDiagnostic(master, kDiagnosticIndex,
                                     kDiagnosticSourceNodeSubindex,
                                     current.source_node_id,
                                     "0x2301 last_source_node_id")
            || !readRemoteDiagnostic(master, kDiagnosticIndex,
                                     kDiagnosticCobIdSubindex, current.cob_id,
                                     "0x2301 last_cob_id")
            || !readRemoteDiagnostic(master, kDiagnosticIndex,
                                     kDiagnosticErrorCodeSubindex,
                                     current.error_code,
                                     "0x2301 last_error_code")
            || !readRemoteDiagnostic(master, kDiagnosticIndex,
                                     kDiagnosticErrorRegisterSubindex,
                                     current.error_register,
                                     "0x2301 last_error_register")
            || !readRemoteDiagnostic(master, kDiagnosticIndex,
                                     kDiagnosticErrorBitSubindex,
                                     current.error_bit,
                                     "0x2301 last_error_bit")
            || !readRemoteDiagnostic(master, kDiagnosticIndex,
                                     kDiagnosticInfoCodeSubindex,
                                     current.info_code,
                                     "0x2301 last_info_code")
            || !readRemoteDiagnostic(master, kDiagnosticIndex,
                                     kDiagnosticCountSubindex, count_after,
                                     "0x2301 remote_rx_count after")) {
            return false;
        }

        if (count_before == count_after) {
            current.rx_count = count_after;
            snapshot = current;
            return true;
        }

        spdlog::warn(
            "B06 diagnostic changed during SDO snapshot: before={} after={} attempt={}/{}",
            count_before, count_after, attempt + 1U, kSnapshotRetryCount);
    }

    spdlog::error("B06 diagnostic snapshot remained unstable");
    return false;
}

/**
 * @brief Wait for exactly one additional MCU EMCY callback publication.
 */
bool waitForNextRxCount(lely::canopen::AsyncMaster& master,
                        std::uint32_t baseline_count,
                        std::uint32_t& observed_count)
{
    const std::uint32_t expected_count = baseline_count + 1U;
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(kEmcyObservationTimeoutMs);

    do {
        std::uint32_t current_count = 0;
        if (!readRemoteDiagnostic(master, kDiagnosticIndex,
                                  kDiagnosticCountSubindex, current_count,
                                  "0x2301 remote_rx_count poll")) {
            return false;
        }
        if (current_count == expected_count) {
            observed_count = current_count;
            return true;
        }
        if (current_count != baseline_count) {
            spdlog::error(
                "B06 unexpected EMCY callback count change: baseline={} expected={} actual={}",
                baseline_count, expected_count, current_count);
            return false;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kDiagnosticPollIntervalMs));
    } while (std::chrono::steady_clock::now() < deadline);

    spdlog::error("B06 EMCY callback timed out: baseline={} expected={}",
                  baseline_count, expected_count);
    return false;
}

/**
 * @brief Validate a diagnostic snapshot against one nonzero EMCY vector.
 */
bool validateVectorSnapshot(const EmcyConsumerSnapshot& snapshot,
                            std::uint32_t expected_count,
                            const EmcyTestVector& vector)
{
    bool result = true;
    if (snapshot.rx_count != expected_count) {
        spdlog::error("B06 vector {} count mismatch: expected={} actual={}",
                      vector.name, expected_count, snapshot.rx_count);
        result = false;
    }
    if (snapshot.source_node_id != CANOPEN_MASTER_NODE_ID) {
        spdlog::error(
            "B06 vector {} source mismatch: expected={} actual={}",
            vector.name, CANOPEN_MASTER_NODE_ID,
            static_cast<unsigned int>(snapshot.source_node_id));
        result = false;
    }
    if (snapshot.cob_id != kExpectedHostEmcyCanId) {
        spdlog::error(
            "B06 vector {} COB-ID mismatch: expected=0x{:03x} actual=0x{:03x}",
            vector.name, kExpectedHostEmcyCanId,
            static_cast<unsigned int>(snapshot.cob_id));
        result = false;
    }
    if (snapshot.error_code != vector.error_code
        || snapshot.error_register != vector.error_register
        || snapshot.error_bit != vector.expected_error_bit
        || snapshot.info_code != vector.expected_info_code) {
        spdlog::error(
            "B06 vector {} payload mismatch: code=0x{:04x} register=0x{:02x} bit=0x{:02x} info=0x{:08x}",
            vector.name, static_cast<unsigned int>(snapshot.error_code),
            static_cast<unsigned int>(snapshot.error_register),
            static_cast<unsigned int>(snapshot.error_bit), snapshot.info_code);
        result = false;
    }
    return result;
}

/**
 * @brief Validate the standard error-reset/no-error EMCY diagnostic.
 */
bool validateRecoverySnapshot(const EmcyConsumerSnapshot& snapshot,
                              std::uint32_t expected_count)
{
    if (snapshot.rx_count != expected_count
        || snapshot.source_node_id != CANOPEN_MASTER_NODE_ID
        || snapshot.cob_id != kExpectedHostEmcyCanId
        || snapshot.error_code != 0U || snapshot.error_register != 0U
        || snapshot.error_bit != 0U || snapshot.info_code != 0U) {
        spdlog::error(
            "B06 recovery mismatch: count={} source={} cob=0x{:03x} code=0x{:04x} register=0x{:02x} bit=0x{:02x} info=0x{:08x}",
            snapshot.rx_count,
            static_cast<unsigned int>(snapshot.source_node_id),
            static_cast<unsigned int>(snapshot.cob_id),
            static_cast<unsigned int>(snapshot.error_code),
            static_cast<unsigned int>(snapshot.error_register),
            static_cast<unsigned int>(snapshot.error_bit), snapshot.info_code);
        return false;
    }
    return true;
}

/**
 * @brief Require the MCU atomic EMCY diagnostic to survive communication reset unchanged.
 */
bool validateResetPreservedSnapshot(const EmcyConsumerSnapshot& before,
                                    const EmcyConsumerSnapshot& after)
{
    if (after.rx_count == before.rx_count
        && after.source_node_id == before.source_node_id
        && after.cob_id == before.cob_id
        && after.error_code == before.error_code
        && after.error_register == before.error_register
        && after.error_bit == before.error_bit
        && after.info_code == before.info_code) {
        return true;
    }

    spdlog::error("B06-08 MCU diagnostic changed across Reset Communication");
    spdlog::error(
        "B06-08 before: count={} source={} cob=0x{:03x} code=0x{:04x} register=0x{:02x} bit=0x{:02x} info=0x{:08x}",
        before.rx_count, static_cast<unsigned int>(before.source_node_id),
        static_cast<unsigned int>(before.cob_id),
        static_cast<unsigned int>(before.error_code),
        static_cast<unsigned int>(before.error_register),
        static_cast<unsigned int>(before.error_bit), before.info_code);
    spdlog::error(
        "B06-08 after: count={} source={} cob=0x{:03x} code=0x{:04x} register=0x{:02x} bit=0x{:02x} info=0x{:08x}",
        after.rx_count, static_cast<unsigned int>(after.source_node_id),
        static_cast<unsigned int>(after.cob_id),
        static_cast<unsigned int>(after.error_code),
        static_cast<unsigned int>(after.error_register),
        static_cast<unsigned int>(after.error_bit), after.info_code);
    return false;
}

/**
 * @brief Emit one nonzero EMCY through the existing Host Lely producer.
 */
bool emitTestEmcy(EmcyTestMaster& master,
                  EmcyConsumerRuntimeState& state,
                  const EmcyTestVector& vector)
{
    bool stack_updated = false;
    const int push_result = master.pushLocalEmcy(
        vector.error_code, vector.error_register, vector.msef.data(),
        stack_updated);

    /* A failed push may still have inserted the error before its frame buffer
     * allocation failed. Keep cleanup ownership aligned with the real stack. */
    if (stack_updated) {
        state.host_error_active = true;
        ++state.expected_host_error_count;
    }

    if (push_result == -1) {
        spdlog::error(
            "B06 failed to push vector {} into the Host EMCY producer; local_stack_updated={}",
            vector.name, stack_updated);
        return false;
    }

    spdlog::info(
        "B06 emitted vector {}: code=0x{:04x} register=0x{:02x} bit=0x{:02x} info=0x{:08x}",
        vector.name, static_cast<unsigned int>(vector.error_code),
        static_cast<unsigned int>(vector.error_register),
        static_cast<unsigned int>(vector.expected_error_bit),
        vector.expected_info_code);
    return true;
}

/**
 * @brief Clear the Host Lely EMCY stack and emit standard recovery when active.
 */
bool clearLocalEmcy(EmcyTestMaster& master,
                    EmcyConsumerRuntimeState& state)
{
    std::uint8_t history_count = 0;
    if (!readLocalObject(master, kLocalErrorHistoryIndex, kScalarSubindex,
                         history_count, "Pre-defined Error Field count during cleanup")) {
        return false;
    }
    if (static_cast<unsigned int>(history_count)
        != state.expected_host_error_count) {
        spdlog::error(
            "B06 refuses to clear Host EMCY stack with unexpected depth: expected={} actual={}",
            state.expected_host_error_count,
            static_cast<unsigned int>(history_count));
        return false;
    }

    std::uint16_t active_error = 0;
    std::uint8_t active_register = 0;
    if (!master.peekLocalEmcy(active_error, active_register)) {
        spdlog::error("B06 Host Lely EMCY service is unavailable during cleanup");
        return false;
    }
    if (active_error == 0U && active_register == 0U) {
        state.host_error_active = false;
        state.expected_host_error_count = 0U;
        return true;
    }
    if ((active_error != kErrorCodeA && active_error != kErrorCodeB)
        || active_register != kErrorRegister) {
        spdlog::error(
            "B06 refuses to clear an unexpected Host EMCY: code=0x{:04x} register=0x{:02x}",
            static_cast<unsigned int>(active_error),
            static_cast<unsigned int>(active_register));
        return false;
    }

    const int clear_result = master.clearLocalEmcy();
    /* Lely clears its stack and 0x1003 before attempting recovery transmit,
     * so the Host error is no longer active even when that transmit fails. */
    state.host_error_active = false;
    state.expected_host_error_count = 0U;
    if (clear_result == -1) {
        spdlog::error(
            "B06 Host EMCY stack was cleared but the recovery transmission failed");
        return false;
    }
    return true;
}

/**
 * @brief Emit one vector and require one coherent matching MCU publication.
 */
bool emitAndValidateVector(EmcyTestMaster& master,
                           EmcyConsumerRuntimeState& state,
                           EmcyConsumerSnapshot& current,
                           const EmcyTestVector& vector,
                           const char* phase)
{
    const std::uint32_t baseline_count = current.rx_count;
    if (!emitTestEmcy(master, state, vector)) {
        spdlog::error("{} Host EMCY push failed", phase);
        return false;
    }

    std::uint32_t observed_count = 0;
    if (!waitForNextRxCount(master, baseline_count, observed_count)) {
        spdlog::error("{} did not observe exactly one EMCY callback", phase);
        return false;
    }

    EmcyConsumerSnapshot next;
    if (!readStableSnapshot(master, next)
        || !validateVectorSnapshot(next, observed_count, vector)) {
        spdlog::error("{} diagnostic validation failed", phase);
        return false;
    }

    current = next;
    spdlog::info("{} passed: rx_count={}", phase, current.rx_count);
    return true;
}

/**
 * @brief Clear active Host errors and require one matching recovery callback.
 */
bool clearAndValidateRecovery(EmcyTestMaster& master,
                              EmcyConsumerRuntimeState& state,
                              EmcyConsumerSnapshot& current,
                              const char* phase)
{
    const std::uint32_t baseline_count = current.rx_count;
    if (!state.host_error_active) {
        spdlog::error("{} requires an active Host EMCY before recovery", phase);
        return false;
    }
    if (!clearLocalEmcy(master, state)) {
        return false;
    }

    std::uint32_t observed_count = 0;
    if (!waitForNextRxCount(master, baseline_count, observed_count)) {
        spdlog::error("{} did not observe the recovery EMCY", phase);
        return false;
    }

    EmcyConsumerSnapshot next;
    if (!readStableSnapshot(master, next)
        || !validateRecoverySnapshot(next, observed_count)) {
        spdlog::error("{} recovery diagnostic validation failed", phase);
        return false;
    }

    current = next;
    spdlog::info("{} passed: rx_count={}", phase, current.rx_count);
    return true;
}

/**
 * @brief Verify that ordinary MCU SDO service remains healthy after EMCY callbacks.
 */
bool validateSdoHealth(lely::canopen::AsyncMaster& master)
{
    std::uint32_t device_type = 0;
    if (readRemoteSdo<std::uint32_t>(
            master, CANOPEN_SLAVE_NODE_ID, 0x1000U, kScalarSubindex,
            device_type, std::chrono::milliseconds(kSdoTimeoutMs),
            std::chrono::milliseconds(kSdoCompletionMarginMs))
        != SdoOperationResult::SUCCESS) {
        spdlog::error("B06-07 ordinary SDO health read failed");
        return false;
    }
    spdlog::info("B06-07 SDO health passed: 0x1000=0x{:08x}", device_type);
    return true;
}

/**
 * @brief Reset MCU communication and prove the EMCY callback is rebound.
 */
bool validateResetRebind(EmcyTestMaster& master,
                         EmcyConsumerRuntimeState& state,
                         EmcyConsumerSnapshot& current)
{
    if (state.host_error_active) {
        spdlog::error("B06-08 expected a clean Host EMCY stack before the reset marker");
        return false;
    }
    if (!emitAndValidateVector(master, state, current, kVectorA,
                               "B06-08 pre-reset persistence marker")) {
        return false;
    }

    const EmcyConsumerSnapshot pre_reset = current;
    prepareBootWait();
    if (!issueNmtCommand(master, lely::canopen::NmtCommand::RESET_COMM,
                         CANOPEN_SLAVE_NODE_ID,
                         "B06 Reset Communication")) {
        return false;
    }
    state.reset_communication_issued = true;
    state.remote_operational_restored = false;

    lely::canopen::NmtState boot_state = lely::canopen::NmtState::BOOTUP;
    if (!waitForBootCompletion(
            std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS), boot_state)) {
        spdlog::error("B06-08 MCU Boot after Reset Communication timed out");
        return false;
    }
    state.remote_operational_restored =
        boot_state == lely::canopen::NmtState::START;
    if (state.remote_operational_restored) {
        spdlog::info(
            "B06-08 Boot already confirmed MCU Operational; final NMT Start is not required");
    }

    EmcyConsumerSnapshot post_reset;
    if (!readStableSnapshot(master, post_reset)) {
        spdlog::error("B06-08 post-reset diagnostic snapshot is unavailable");
        return false;
    }
    if (!validateResetPreservedSnapshot(pre_reset, post_reset)) {
        return false;
    }
    current = post_reset;
    spdlog::info(
        "B06-08 MCU diagnostic survived Reset Communication: rx_count={}",
        current.rx_count);

    if (!emitAndValidateVector(master, state, current, kVectorB,
                               "B06-08 post-reset EMCY")
        || !clearAndValidateRecovery(master, state, current,
                                     "B06-08 post-reset recovery")) {
        return false;
    }

    if (!state.remote_operational_restored) {
        if (!issueNmtCommandAndWaitForState(
                master, lely::canopen::NmtCommand::START,
                CANOPEN_SLAVE_NODE_ID, lely::canopen::NmtState::START,
                std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS),
                "B06 final NMT Start")) {
            spdlog::error("B06-08 could not restore MCU Operational state");
            return false;
        }
        state.remote_operational_restored = true;
    }
    spdlog::info("B06-08 Reset Communication callback rebind passed");
    return true;
}

/**
 * @brief Run all first-version B06 protocol checks after preflight.
 */
bool runEmcyConsumerValidation(EmcyTestMaster& master,
                               EmcyConsumerRuntimeState& state)
{
    EmcyConsumerSnapshot current;
    if (!readStableSnapshot(master, current)) {
        spdlog::error(
            "B06 MCU diagnostic 0x2301 is unavailable; enable the EMCY consumer diagnostic firmware option");
        return false;
    }
    spdlog::info("B06 diagnostic baseline: rx_count={}", current.rx_count);

    if (!emitAndValidateVector(master, state, current, kVectorA,
                               "B06-01/02 single EMCY and MSEF")
        || !emitAndValidateVector(master, state, current, kVectorB,
                                  "B06-03 consecutive EMCY")) {
        return false;
    }

    if (!clearAndValidateRecovery(master, state, current,
                                  "B06-05 recovery")) {
        return false;
    }

    if (!emitAndValidateVector(master, state, current, kVectorA,
                               "B06-04 duplicate EMCY first")
        || !emitAndValidateVector(master, state, current, kVectorA,
                                  "B06-04 duplicate EMCY second")
        || !clearAndValidateRecovery(master, state, current,
                                     "B06-04 duplicate cleanup")) {
        return false;
    }

    if (!validateSdoHealth(master)) {
        return false;
    }

    return validateResetRebind(master, state, current);
}

/**
 * @brief Restore B06-owned Host/MCU state after success or failure.
 */
bool cleanupEmcyConsumerValidation(EmcyTestMaster& master,
                                   EmcyConsumerRuntimeState& state)
{
    bool result = true;

    if (state.host_error_active && !clearLocalEmcy(master, state)) {
        result = false;
    }

    if (state.reset_communication_issued
        && !state.remote_operational_restored) {
        if (!issueNmtCommandAndWaitForState(
                master, lely::canopen::NmtCommand::START,
                CANOPEN_SLAVE_NODE_ID, lely::canopen::NmtState::START,
                std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS),
                "B06 cleanup NMT Start")) {
            spdlog::error("B06 cleanup could not restore MCU Operational state");
            result = false;
        } else {
            state.remote_operational_restored = true;
        }
    }

    return result;
}

} // namespace

int emcyConsumerProcess(EmcyTestMaster& master)
{
    if (!validateProducerPreflight(master)) {
        return 1;
    }

    EmcyConsumerRuntimeState state;
    int result = runEmcyConsumerValidation(master, state) ? 0 : 1;

    if (!cleanupEmcyConsumerValidation(master, state)) {
        result = 1;
    }

    if (result == 0) {
        spdlog::info(
            "B06 EMCY consumer delivery, duplicate, recovery, SDO health, and reset rebind passed");
    }
    return result;
}
