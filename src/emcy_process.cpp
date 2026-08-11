/**
 * @file
 * @brief Implements A06 EMCY producer validation.
 */

#include "emcy_process.h"

#include "canopen_config.h"
#include "canopen_emcy.h"
#include "canopen_sdo.h"

#include <lely/coapp/master.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <system_error>
#include <thread>
#include <vector>

namespace {

/** CiA 301 error register. */
constexpr std::uint16_t kErrorRegisterIndex = 0x1001;
/** Pre-defined error field. */
constexpr std::uint16_t kErrorHistoryIndex = 0x1003;
/** Configurable EMCY producer COB-ID. */
constexpr std::uint16_t kEmcyCobIdIndex = 0x1014;
/** EMCY inhibit time in 100 us units. */
constexpr std::uint16_t kEmcyInhibitIndex = 0x1015;
/** Local Producer Heartbeat time used as the deterministic fault source. */
constexpr std::uint16_t kHeartbeatIndex = 0x1017;
/** Master-local Emergency consumer object. */
constexpr std::uint16_t kEmcyConsumerIndex = 0x1028;
/** All scalar objects used by A06 are at sub-index zero. */
constexpr std::uint8_t kScalarSubindex = 0x00;
/** Lely maps 0x1028 sub-index N to remote EMCY producer node-ID N. */
constexpr std::uint8_t kEmcyConsumerSubindex = CANOPEN_SLAVE_NODE_ID;

/** CANopen invalid/disable bit used by 0x1014 and 0x1028. */
constexpr std::uint32_t kCobIdInvalidMask = 0x80000000U;
/** Standard 11-bit CAN-ID field used by the current test topology. */
constexpr std::uint32_t kStandardCanIdMask = 0x000007FFU;
/** A06 temporary standard EMCY CAN-ID; 0x681 is free in the current topology. */
constexpr std::uint32_t kTestEmcyCobId = 0x00000681U;

/** Heartbeat consumer timeout EMCY produced by CANopenNode. */
constexpr std::uint16_t kHeartbeatConsumerEmcyCode = 0x8130;
/** EMCY code zero reports recovery of an active error. */
constexpr std::uint16_t kEmcyResetCode = 0x0000;
/** Error register bit 4 denotes communication errors. */
constexpr std::uint8_t kCommunicationErrorRegisterBit = 0x10;
/** CANopenNode error-status bit for Heartbeat consumer timeout. */
constexpr std::uint8_t kHeartbeatConsumerErrorBit = 0x1B;
/** Current demo OD exposes 16 entries at 0x1003:01..10. */
constexpr std::uint8_t kErrorHistoryCapacity = 16U;

/** Allow enough time for the 1500 ms consumer timeout plus scheduling margin. */
constexpr std::uint32_t kEmcyWaitTimeoutMs = 5000U;
/** Ensure the consumer has observed several normal producer heartbeats first. */
constexpr std::uint32_t kHeartbeatSettleMs = 2500U;
/** Same active CANopenNode error must not continuously regenerate EMCY. */
constexpr std::uint32_t kDuplicateGuardMs = 1000U;
/** 15000 * 100 us = 1500 ms test inhibit interval. */
constexpr std::uint16_t kTestInhibitTime100us = 15000U;
/** Timestamp lower-bound tolerance for host/RTOS callback scheduling. */
constexpr std::uint32_t kInhibitToleranceMs = 100U;
/** Upper margin includes one producer heartbeat interval plus scheduling slack. */
constexpr std::uint32_t kInhibitUpperMarginMs = 1000U;
/** Cleanup polls 0x1001 while an EMCY callback path may be unavailable. */
constexpr std::uint32_t kCleanupPollIntervalMs = 100U;

/** Values that must be restored after any A06 exit path. */
struct EmcySavedState {
    /** Original local Producer Heartbeat period; non-zero is required by A06. */
    std::uint16_t master_heartbeat = 0;
    /** Original master-local 0x1028 entry for the slave. */
    std::uint32_t master_consumer_cob_id = 0;
    /** Original slave 0x1014 value. */
    std::uint32_t slave_emcy_cob_id = 0;
    /** Original slave 0x1015 value. */
    std::uint16_t slave_inhibit_time = 0;
};

/** Snapshot of the diagnostic fields A06 reads from 0x1001/0x1003. */
struct ErrorHistorySnapshot {
    /** Current 0x1001 Error Register. */
    std::uint8_t error_register = 0;
    /** Number of valid 0x1003 history entries. */
    std::uint8_t count = 0;
    /** 0x1003:01 when count is non-zero. */
    std::uint32_t newest = 0;
    /** 0x1003:02 when at least two entries are present. */
    std::uint32_t previous = 0;
};

/**
 * @brief Read a master-local OD value with uniform diagnostics.
 */
template <class T>
bool readLocalObject(lely::canopen::AsyncMaster& master, std::uint16_t index,
                     std::uint8_t subindex, T& value, const char* label)
{
    std::error_code error;
    const T read_value = master.Read<T>(index, subindex, error);
    if (error) {
        spdlog::error("Unable to read local {} (0x{:04x}:{:02x}): {}",
                      label, index, static_cast<unsigned int>(subindex),
                      error.message());
        return false;
    }
    value = read_value;
    return true;
}

/**
 * @brief Write a master-local OD value with uniform diagnostics.
 */
template <class T>
bool writeLocalObject(lely::canopen::AsyncMaster& master, std::uint16_t index,
                      std::uint8_t subindex, T value, const char* label)
{
    std::error_code error;
    master.Write<T>(index, subindex, value, error);
    if (error) {
        spdlog::error("Unable to write local {} (0x{:04x}:{:02x}): {}",
                      label, index, static_cast<unsigned int>(subindex),
                      error.message());
        return false;
    }
    return true;
}

/**
 * @brief Read a slave OD value through the common asynchronous SDO adapter.
 */
template <class T>
bool readRemoteObject(lely::canopen::AsyncMaster& master, std::uint16_t index,
                      std::uint8_t subindex, T& value, const char* label)
{
    if (readRemoteSdo<T>(master, CANOPEN_SLAVE_NODE_ID, index, subindex,
                         value)
        != SdoOperationResult::SUCCESS) {
        spdlog::error("A06 remote read failed: {}", label);
        return false;
    }
    return true;
}

/**
 * @brief Write a slave OD value through the common asynchronous SDO adapter.
 */
template <class T>
bool writeRemoteObject(lely::canopen::AsyncMaster& master,
                       std::uint16_t index, std::uint8_t subindex, T value,
                       const char* label)
{
    if (writeRemoteSdo<T>(master, CANOPEN_SLAVE_NODE_ID, index, subindex,
                          value)
        != SdoOperationResult::SUCCESS) {
        spdlog::error("A06 remote write failed: {}", label);
        return false;
    }
    return true;
}

/** @return true when the COB-ID represents an enabled EMCY path. */
bool cobIdEnabled(std::uint32_t cob_id)
{
    return (cob_id & kCobIdInvalidMask) == 0U
           && (cob_id & kStandardCanIdMask) != 0U;
}

/** @return Standard CAN-ID field from an EMCY communication value. */
std::uint32_t standardCanId(std::uint32_t cob_id)
{
    return cob_id & kStandardCanIdMask;
}

/**
 * @brief Read 0x1001 and the newest two entries of 0x1003.
 */
bool readErrorHistory(lely::canopen::AsyncMaster& master,
                      ErrorHistorySnapshot& snapshot)
{
    if (!readRemoteObject(master, kErrorRegisterIndex, kScalarSubindex,
                          snapshot.error_register, "Error Register")) {
        return false;
    }
    if (!readRemoteObject(master, kErrorHistoryIndex, kScalarSubindex,
                          snapshot.count, "error history count")) {
        return false;
    }
    if (snapshot.count > kErrorHistoryCapacity) {
        spdlog::error("Unexpected 0x1003 history count: {} > {}",
                      static_cast<unsigned int>(snapshot.count),
                      static_cast<unsigned int>(kErrorHistoryCapacity));
        return false;
    }
    if (snapshot.count > 0U
        && !readRemoteObject(master, kErrorHistoryIndex, 0x01,
                             snapshot.newest, "newest error history")) {
        return false;
    }
    if (snapshot.count > 1U
        && !readRemoteObject(master, kErrorHistoryIndex, 0x02,
                             snapshot.previous, "previous error history")) {
        return false;
    }
    return true;
}

/** @return Error code stored in bytes 0..1 of a CANopenNode history entry. */
std::uint16_t historyErrorCode(std::uint32_t entry)
{
    return static_cast<std::uint16_t>(entry & 0x0000FFFFU);
}

/** @return Error Register stored in byte 2 of a CANopenNode history entry. */
std::uint8_t historyErrorRegister(std::uint32_t entry)
{
    return static_cast<std::uint8_t>((entry >> 16U) & 0xFFU);
}

/** @return CANopenNode error-status bit stored in byte 3 of history. */
std::uint8_t historyErrorBit(std::uint32_t entry)
{
    return static_cast<std::uint8_t>((entry >> 24U) & 0xFFU);
}

/**
 * @brief Calculate the expected bounded 0x1003 count after new events.
 */
std::uint8_t expectedHistoryCount(std::uint8_t baseline,
                                  std::uint8_t added_events)
{
    const unsigned int total = static_cast<unsigned int>(baseline)
                               + static_cast<unsigned int>(added_events);
    return static_cast<std::uint8_t>(
        std::min<unsigned int>(total, kErrorHistoryCapacity));
}

/**
 * @brief Validate one CANopenNode 0x1003 entry against the corresponding EMCY.
 */
bool validateHistoryEntry(std::uint32_t entry, std::uint16_t expected_code,
                          std::uint8_t expected_register,
                          const char* phase)
{
    const std::uint16_t code = historyErrorCode(entry);
    const std::uint8_t error_register = historyErrorRegister(entry);
    const std::uint8_t error_bit = historyErrorBit(entry);
    if (code != expected_code || error_register != expected_register
        || error_bit != kHeartbeatConsumerErrorBit) {
        spdlog::error(
            "{} history mismatch: entry=0x{:08x} code=0x{:04x} "
            "error_register=0x{:02x} error_bit=0x{:02x}",
            phase, entry, static_cast<unsigned int>(code),
            static_cast<unsigned int>(error_register),
            static_cast<unsigned int>(error_bit));
        return false;
    }
    return true;
}

/**
 * @brief Validate a Heartbeat-consumer EMCY against CANopenNode-specific data.
 */
bool validateHeartbeatEmcyData(const CanopenEmcyEvent& event,
                               std::uint16_t expected_code,
                               const char* phase)
{
    if (event.error_code != expected_code) {
        spdlog::error("{} EMCY code mismatch: expected=0x{:04x} actual=0x{:04x}",
                      phase, static_cast<unsigned int>(expected_code),
                      static_cast<unsigned int>(event.error_code));
        return false;
    }
    if (event.manufacturer_data[0] != kHeartbeatConsumerErrorBit
        || event.manufacturer_data[1] != 0U
        || event.manufacturer_data[2] != 0U
        || event.manufacturer_data[3] != 0U
        || event.manufacturer_data[4] != 0U) {
        spdlog::error(
            "{} CANopenNode EMCY manufacturer field mismatch: "
            "{:02x} {:02x} {:02x} {:02x} {:02x}",
            phase,
            static_cast<unsigned int>(event.manufacturer_data[0]),
            static_cast<unsigned int>(event.manufacturer_data[1]),
            static_cast<unsigned int>(event.manufacturer_data[2]),
            static_cast<unsigned int>(event.manufacturer_data[3]),
            static_cast<unsigned int>(event.manufacturer_data[4]));
        return false;
    }
    return true;
}

/**
 * @brief Validate EMCY Error Register against remote 0x1001.
 */
bool validateErrorRegister(lely::canopen::AsyncMaster& master,
                           const CanopenEmcyEvent& event,
                           bool communication_error_expected,
                           const char* phase)
{
    std::uint8_t error_register = 0;
    if (!readRemoteObject(master, kErrorRegisterIndex, kScalarSubindex,
                          error_register, "Error Register")) {
        return false;
    }
    if (error_register != event.error_register) {
        spdlog::error(
            "{} Error Register mismatch: EMCY=0x{:02x} OD=0x{:02x}",
            phase, static_cast<unsigned int>(event.error_register),
            static_cast<unsigned int>(error_register));
        return false;
    }
    if (communication_error_expected) {
        if ((error_register & kCommunicationErrorRegisterBit) == 0U) {
            spdlog::error(
                "{} Error Register does not contain communication bit: 0x{:02x}",
                phase, static_cast<unsigned int>(error_register));
            return false;
        }
    } else if (error_register != 0U) {
        spdlog::error("{} Error Register did not clear: 0x{:02x}", phase,
                      static_cast<unsigned int>(error_register));
        return false;
    }
    return true;
}

/**
 * @brief Write the local Producer Heartbeat time used for fault injection.
 */
bool writeMasterHeartbeat(lely::canopen::AsyncMaster& master,
                          std::uint16_t heartbeat_ms)
{
    if (!writeLocalObject(master, kHeartbeatIndex, kScalarSubindex,
                          heartbeat_ms, "Producer Heartbeat")) {
        return false;
    }
    spdlog::info("A06 master Producer Heartbeat set to {} ms",
                 static_cast<unsigned int>(heartbeat_ms));
    return true;
}

/**
 * @brief Stop the master heartbeat and wait for a fresh 0x8130 EMCY.
 */
bool triggerHeartbeatFault(lely::canopen::AsyncMaster& master,
                           CanopenEmcyEvent& event)
{
    const std::uint64_t sequence = snapshotCanopenEmcySequence();
    if (!writeMasterHeartbeat(master, 0U)) {
        return false;
    }
    if (!waitForCanopenEmcyEvent(
            sequence, CANOPEN_SLAVE_NODE_ID, kHeartbeatConsumerEmcyCode,
            std::chrono::milliseconds(kEmcyWaitTimeoutMs), event)) {
        spdlog::error("A06 heartbeat-consumer EMCY 0x8130 timed out");
        return false;
    }
    return true;
}

/**
 * @brief Restore the master heartbeat and wait for a fresh EMCY reset.
 */
bool recoverHeartbeatFault(lely::canopen::AsyncMaster& master,
                           std::uint16_t heartbeat_ms,
                           std::chrono::milliseconds timeout,
                           CanopenEmcyEvent& event)
{
    const std::uint64_t sequence = snapshotCanopenEmcySequence();
    if (!writeMasterHeartbeat(master, heartbeat_ms)) {
        return false;
    }
    if (!waitForCanopenEmcyEvent(
            sequence, CANOPEN_SLAVE_NODE_ID, kEmcyResetCode, timeout,
            event)) {
        spdlog::error("A06 heartbeat-consumer EMCY reset timed out");
        return false;
    }
    return true;
}

/**
 * @brief Disable the current slave EMCY producer without changing its CAN-ID.
 */
bool disableRemoteEmcyProducer(lely::canopen::AsyncMaster& master)
{
    std::uint32_t current = 0;
    if (!readRemoteObject(master, kEmcyCobIdIndex, kScalarSubindex, current,
                          "COB-ID EMCY")) {
        return false;
    }
    if (!cobIdEnabled(current)) {
        return true;
    }

    const std::uint32_t disabled = current | kCobIdInvalidMask;
    if (!writeRemoteObject(master, kEmcyCobIdIndex, kScalarSubindex, disabled,
                           "disabled COB-ID EMCY")) {
        return false;
    }

    std::uint32_t readback = 0;
    if (!readRemoteObject(master, kEmcyCobIdIndex, kScalarSubindex, readback,
                          "disabled COB-ID EMCY readback")) {
        return false;
    }
    if (readback != disabled) {
        spdlog::error(
            "Slave EMCY disable readback mismatch: expected=0x{:08x} "
            "actual=0x{:08x}",
            disabled, readback);
        return false;
    }
    return true;
}

/**
 * @brief Switch the master-local 0x1028 entry using disable/change semantics.
 */
bool switchLocalEmcyConsumerCobId(lely::canopen::AsyncMaster& master,
                                  std::uint32_t target)
{
    std::uint32_t current = 0;
    if (!readLocalObject(master, kEmcyConsumerIndex, kEmcyConsumerSubindex,
                         current, "Emergency consumer COB-ID")) {
        return false;
    }
    if (current == target) {
        return true;
    }

    if (cobIdEnabled(current)
        && standardCanId(current) != standardCanId(target)) {
        const std::uint32_t disabled = current | kCobIdInvalidMask;
        if (!writeLocalObject(master, kEmcyConsumerIndex,
                              kEmcyConsumerSubindex, disabled,
                              "disabled Emergency consumer COB-ID")) {
            return false;
        }
    }

    if (!writeLocalObject(master, kEmcyConsumerIndex, kEmcyConsumerSubindex,
                          target, "Emergency consumer COB-ID")) {
        return false;
    }

    std::uint32_t readback = 0;
    if (!readLocalObject(master, kEmcyConsumerIndex, kEmcyConsumerSubindex,
                         readback, "Emergency consumer COB-ID readback")) {
        return false;
    }
    if (readback != target) {
        spdlog::error(
            "Master EMCY consumer readback mismatch: expected=0x{:08x} "
            "actual=0x{:08x}",
            target, readback);
        return false;
    }
    return true;
}

/**
 * @brief Enable the slave EMCY producer on a target value after it is disabled.
 */
bool writeRemoteEmcyCobId(lely::canopen::AsyncMaster& master,
                          std::uint32_t target)
{
    if (!writeRemoteObject(master, kEmcyCobIdIndex, kScalarSubindex, target,
                           "COB-ID EMCY")) {
        return false;
    }

    std::uint32_t readback = 0;
    if (!readRemoteObject(master, kEmcyCobIdIndex, kScalarSubindex, readback,
                          "COB-ID EMCY readback")) {
        return false;
    }
    if (readback != target) {
        spdlog::error(
            "Slave EMCY COB-ID readback mismatch: expected=0x{:08x} "
            "actual=0x{:08x}; verify EM_PROD_CONFIGURABLE is enabled",
            target, readback);
        return false;
    }
    return true;
}

/**
 * @brief Capture every communication value modified by A06.
 */
bool captureSavedState(lely::canopen::AsyncMaster& master,
                       EmcySavedState& state)
{
    if (!readLocalObject(master, kHeartbeatIndex, kScalarSubindex,
                         state.master_heartbeat,
                         "Producer Heartbeat baseline")) {
        return false;
    }
    if (!readLocalObject(master, kEmcyConsumerIndex, kEmcyConsumerSubindex,
                         state.master_consumer_cob_id,
                         "Emergency consumer COB-ID baseline")) {
        return false;
    }
    if (!readRemoteObject(master, kEmcyCobIdIndex, kScalarSubindex,
                          state.slave_emcy_cob_id,
                          "COB-ID EMCY baseline")) {
        return false;
    }
    if (!readRemoteObject(master, kEmcyInhibitIndex, kScalarSubindex,
                          state.slave_inhibit_time,
                          "EMCY inhibit baseline")) {
        return false;
    }

    if (state.master_heartbeat == 0U) {
        spdlog::error("A06 requires a non-zero master Producer Heartbeat");
        return false;
    }
    if (!cobIdEnabled(state.master_consumer_cob_id)
        || !cobIdEnabled(state.slave_emcy_cob_id)) {
        spdlog::error(
            "A06 requires enabled standard EMCY paths: master=0x{:08x} "
            "slave=0x{:08x}",
            state.master_consumer_cob_id, state.slave_emcy_cob_id);
        return false;
    }
    if (standardCanId(state.master_consumer_cob_id)
        != standardCanId(state.slave_emcy_cob_id)) {
        spdlog::error(
            "Initial EMCY path mismatch: master=0x{:03x} slave=0x{:03x}",
            standardCanId(state.master_consumer_cob_id),
            standardCanId(state.slave_emcy_cob_id));
        return false;
    }
    if (standardCanId(state.slave_emcy_cob_id)
        == standardCanId(kTestEmcyCobId)) {
        spdlog::error("A06 test EMCY CAN-ID 0x{:03x} equals the baseline",
                      standardCanId(kTestEmcyCobId));
        return false;
    }

    spdlog::info(
        "A06 baseline: heartbeat={}ms slave_1014=0x{:08x} "
        "master_1028=0x{:08x} inhibit={} (100us)",
        static_cast<unsigned int>(state.master_heartbeat),
        state.slave_emcy_cob_id, state.master_consumer_cob_id,
        static_cast<unsigned int>(state.slave_inhibit_time));
    return true;
}

/**
 * @brief Validate the current fault and recovery history around one test pair.
 */
bool validateHistoryAfterPair(lely::canopen::AsyncMaster& master,
                              std::uint8_t baseline_count,
                              const CanopenEmcyEvent& fault,
                              const CanopenEmcyEvent& reset,
                              const char* phase)
{
    ErrorHistorySnapshot history;
    if (!readErrorHistory(master, history)) {
        return false;
    }

    /* CANopenNode stores both error-report and error-reset transitions in the
     * EMCY FIFO exposed through 0x1003. One fault/recovery pair therefore adds
     * two entries: reset newest, followed by the preceding fault. */
    const std::uint8_t expected_count =
        expectedHistoryCount(baseline_count, 2U);
    if (history.count != expected_count) {
        spdlog::error("{} history count mismatch: expected={} actual={}",
                      phase, static_cast<unsigned int>(expected_count),
                      static_cast<unsigned int>(history.count));
        return false;
    }
    if (history.count < 2U) {
        spdlog::error("{} history does not contain fault and reset entries",
                      phase);
        return false;
    }
    if (!validateHistoryEntry(history.newest, kEmcyResetCode,
                              reset.error_register, phase)) {
        return false;
    }
    if (!validateHistoryEntry(history.previous, kHeartbeatConsumerEmcyCode,
                              fault.error_register, phase)) {
        return false;
    }
    return true;
}

/**
 * @brief Run full EMCY state/history validation on the original COB-ID.
 */
bool runDefaultEmcyValidation(lely::canopen::AsyncMaster& master,
                              const EmcySavedState& state)
{
    ErrorHistorySnapshot baseline;
    if (!readErrorHistory(master, baseline)) {
        return false;
    }
    if (baseline.error_register != 0U) {
        spdlog::error(
            "A06 precondition failed: Error Register already active: 0x{:02x}",
            static_cast<unsigned int>(baseline.error_register));
        return false;
    }

    /* Isolate basic producer/history behavior from any pre-existing inhibit
     * configuration. The saved value is restored by the common cleanup path. */
    if (!writeRemoteObject(master, kEmcyInhibitIndex, kScalarSubindex,
                           static_cast<std::uint16_t>(0),
                           "zero EMCY inhibit for basic validation")) {
        return false;
    }
    std::uint16_t inhibit_readback = 0;
    if (!readRemoteObject(master, kEmcyInhibitIndex, kScalarSubindex,
                          inhibit_readback,
                          "zero EMCY inhibit readback")) {
        return false;
    }
    if (inhibit_readback != 0U) {
        spdlog::error(
            "A06 basic inhibit readback mismatch: expected=0 actual={}",
            static_cast<unsigned int>(inhibit_readback));
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(kHeartbeatSettleMs));

    CanopenEmcyEvent fault;
    if (!triggerHeartbeatFault(master, fault)) {
        return false;
    }

    bool result = true;
    if (!validateHeartbeatEmcyData(fault, kHeartbeatConsumerEmcyCode,
                                   "A06 default fault")
        || !validateErrorRegister(master, fault, true,
                                  "A06 default fault")) {
        result = false;
    }

    ErrorHistorySnapshot fault_history;
    if (!readErrorHistory(master, fault_history)) {
        result = false;
    } else {
        const std::uint8_t expected_count =
            expectedHistoryCount(baseline.count, 1U);
        if (fault_history.count != expected_count) {
            spdlog::error(
                "A06 default fault history count mismatch: expected={} "
                "actual={}",
                static_cast<unsigned int>(expected_count),
                static_cast<unsigned int>(fault_history.count));
            result = false;
        } else if (fault_history.count == 0U
                   || !validateHistoryEntry(
                       fault_history.newest, kHeartbeatConsumerEmcyCode,
                       fault.error_register, "A06 default fault")) {
            result = false;
        }
    }

    /* Keep the error active and require the shared observer to stay quiet.
     * This checks CANopenNode's error-status transition de-duplication. */
    std::this_thread::sleep_for(std::chrono::milliseconds(kDuplicateGuardMs));
    const std::vector<CanopenEmcyEvent> unexpected =
        getCanopenEmcyEventsAfter(fault.sequence, CANOPEN_SLAVE_NODE_ID);
    if (!unexpected.empty()) {
        for (const CanopenEmcyEvent& event : unexpected) {
            spdlog::error(
                "Unexpected EMCY while heartbeat fault remained active: "
                "seq={} code=0x{:04x} error_register=0x{:02x}",
                event.sequence, static_cast<unsigned int>(event.error_code),
                static_cast<unsigned int>(event.error_register));
        }
        result = false;
    }

    CanopenEmcyEvent reset;
    if (!recoverHeartbeatFault(
            master, state.master_heartbeat,
            std::chrono::milliseconds(kEmcyWaitTimeoutMs), reset)) {
        return false;
    }
    if (!validateHeartbeatEmcyData(reset, kEmcyResetCode,
                                   "A06 default recovery")
        || !validateErrorRegister(master, reset, false,
                                  "A06 default recovery")
        || !validateHistoryAfterPair(master, baseline.count, fault, reset,
                                     "A06 default recovery")) {
        result = false;
    }

    if (result) {
        spdlog::info(
            "A06 default EMCY, 0x1001, 0x1003, duplicate suppression, and "
            "recovery passed");
    }
    return result;
}

/**
 * @brief Move slave 0x1014 and master 0x1028 to the temporary test CAN-ID.
 */
bool configureTestEmcyCobId(lely::canopen::AsyncMaster& master)
{
    /* Disable the producer first so no EMCY can be lost while Lely's consumer
     * receiver is moved from the original CAN-ID to the temporary CAN-ID. */
    if (!disableRemoteEmcyProducer(master)) {
        return false;
    }
    if (!switchLocalEmcyConsumerCobId(master, kTestEmcyCobId)) {
        return false;
    }
    if (!writeRemoteEmcyCobId(master, kTestEmcyCobId)) {
        return false;
    }

    std::uint32_t master_cob_id = 0;
    std::uint32_t slave_cob_id = 0;
    if (!readLocalObject(master, kEmcyConsumerIndex, kEmcyConsumerSubindex,
                         master_cob_id, "test Emergency consumer COB-ID")
        || !readRemoteObject(master, kEmcyCobIdIndex, kScalarSubindex,
                             slave_cob_id, "test COB-ID EMCY")) {
        return false;
    }
    if (master_cob_id != kTestEmcyCobId || slave_cob_id != kTestEmcyCobId) {
        spdlog::error(
            "A06 test EMCY path mismatch: master=0x{:08x} slave=0x{:08x}",
            master_cob_id, slave_cob_id);
        return false;
    }

    spdlog::info("A06 EMCY path switched to CAN-ID 0x{:03x}",
                 standardCanId(kTestEmcyCobId));
    return true;
}

/**
 * @brief Validate 0x1015 while using the dynamically configured EMCY CAN-ID.
 */
bool runConfigurableInhibitValidation(lely::canopen::AsyncMaster& master,
                                      const EmcySavedState& state)
{
    if (!writeRemoteObject(master, kEmcyInhibitIndex, kScalarSubindex,
                           kTestInhibitTime100us, "test EMCY inhibit")) {
        return false;
    }

    std::uint16_t inhibit_readback = 0;
    if (!readRemoteObject(master, kEmcyInhibitIndex, kScalarSubindex,
                          inhibit_readback, "test EMCY inhibit readback")) {
        return false;
    }
    if (inhibit_readback != kTestInhibitTime100us) {
        spdlog::error(
            "A06 inhibit readback mismatch: expected={} actual={}",
            static_cast<unsigned int>(kTestInhibitTime100us),
            static_cast<unsigned int>(inhibit_readback));
        return false;
    }

    ErrorHistorySnapshot baseline;
    if (!readErrorHistory(master, baseline) || baseline.error_register != 0U) {
        spdlog::error(
            "A06 configurable/inhibit phase requires a clear Error Register");
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(kHeartbeatSettleMs));

    CanopenEmcyEvent fault;
    if (!triggerHeartbeatFault(master, fault)) {
        return false;
    }

    bool result = true;
    if (!validateHeartbeatEmcyData(fault, kHeartbeatConsumerEmcyCode,
                                   "A06 configurable fault")
        || !validateErrorRegister(master, fault, true,
                                  "A06 configurable fault")) {
        result = false;
    }

    /* Restore heartbeat immediately. The reset condition becomes active before
     * the inhibit interval expires, so its actual transmission must be delayed
     * relative to the preceding fault EMCY. */
    CanopenEmcyEvent reset;
    if (!recoverHeartbeatFault(
            master, state.master_heartbeat,
            std::chrono::milliseconds(kEmcyWaitTimeoutMs), reset)) {
        return false;
    }
    if (!validateHeartbeatEmcyData(reset, kEmcyResetCode,
                                   "A06 configurable recovery")
        || !validateErrorRegister(master, reset, false,
                                  "A06 configurable recovery")) {
        result = false;
    }

    const auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(
        reset.timestamp - fault.timestamp);
    const std::int64_t interval_ms = interval.count();
    const std::int64_t inhibit_ms =
        static_cast<std::int64_t>(kTestInhibitTime100us) / 10;
    const std::int64_t minimum_ms =
        inhibit_ms - static_cast<std::int64_t>(kInhibitToleranceMs);
    const std::int64_t maximum_ms =
        inhibit_ms + static_cast<std::int64_t>(kInhibitUpperMarginMs);
    if (interval_ms < minimum_ms || interval_ms > maximum_ms) {
        spdlog::error(
            "A06 inhibit timing mismatch: interval={}ms expected={}..{}ms",
            interval_ms, minimum_ms, maximum_ms);
        result = false;
    } else {
        spdlog::info(
            "A06 inhibit timing passed: interval={}ms expected={}..{}ms",
            interval_ms, minimum_ms, maximum_ms);
    }

    if (!validateHistoryAfterPair(master, baseline.count, fault, reset,
                                  "A06 configurable recovery")) {
        result = false;
    }

    return result;
}

/**
 * @brief Poll 0x1001 until the injected communication error has cleared.
 */
bool waitForClearErrorRegister(lely::canopen::AsyncMaster& master)
{
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(kEmcyWaitTimeoutMs);
    do {
        std::uint8_t error_register = 0;
        if (readRemoteObject(master, kErrorRegisterIndex, kScalarSubindex,
                             error_register, "cleanup Error Register")
            && error_register == 0U) {
            return true;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kCleanupPollIntervalMs));
    } while (std::chrono::steady_clock::now() < deadline);

    spdlog::error("A06 cleanup could not confirm Error Register recovery");
    return false;
}

/**
 * @brief Restore all communication values saved before A06.
 */
bool restoreEmcyConfiguration(lely::canopen::AsyncMaster& master,
                              const EmcySavedState& state)
{
    bool result = true;

    /* Heartbeat is restored first so any active injected fault can clear while
     * the current EMCY producer/consumer path is still paired. */
    if (!writeMasterHeartbeat(master, state.master_heartbeat)) {
        result = false;
    }
    if (!waitForClearErrorRegister(master)) {
        result = false;
    }

    if (!writeRemoteObject(master, kEmcyInhibitIndex, kScalarSubindex,
                           state.slave_inhibit_time,
                           "restore EMCY inhibit")) {
        result = false;
    }

    /* Disable the current producer before changing either side's CAN-ID. This
     * is required by CANopenNode 0x1014 and prevents a receiver-switch window. */
    if (!disableRemoteEmcyProducer(master)) {
        result = false;
    }
    if (!switchLocalEmcyConsumerCobId(master,
                                      state.master_consumer_cob_id)) {
        result = false;
    }
    if (!writeRemoteEmcyCobId(master, state.slave_emcy_cob_id)) {
        result = false;
    }

    std::uint16_t heartbeat = 0;
    std::uint16_t inhibit = 0;
    std::uint32_t master_cob_id = 0;
    std::uint32_t slave_cob_id = 0;
    if (!readLocalObject(master, kHeartbeatIndex, kScalarSubindex, heartbeat,
                         "restored Producer Heartbeat")
        || !readLocalObject(master, kEmcyConsumerIndex,
                            kEmcyConsumerSubindex, master_cob_id,
                            "restored Emergency consumer COB-ID")
        || !readRemoteObject(master, kEmcyCobIdIndex, kScalarSubindex,
                             slave_cob_id, "restored COB-ID EMCY")
        || !readRemoteObject(master, kEmcyInhibitIndex, kScalarSubindex,
                             inhibit, "restored EMCY inhibit")) {
        result = false;
    } else if (heartbeat != state.master_heartbeat
               || master_cob_id != state.master_consumer_cob_id
               || slave_cob_id != state.slave_emcy_cob_id
               || inhibit != state.slave_inhibit_time) {
        spdlog::error(
            "A06 restoration mismatch: heartbeat={}/{} master_1028={:08x}/{:08x} "
            "slave_1014={:08x}/{:08x} inhibit={}/{}",
            static_cast<unsigned int>(heartbeat),
            static_cast<unsigned int>(state.master_heartbeat), master_cob_id,
            state.master_consumer_cob_id, slave_cob_id,
            state.slave_emcy_cob_id, static_cast<unsigned int>(inhibit),
            static_cast<unsigned int>(state.slave_inhibit_time));
        result = false;
    }

    if (result) {
        spdlog::info("A06 communication configuration restored and verified");
    }
    return result;
}

/**
 * @brief Verify the restored original EMCY path and ordinary SDO health.
 */
bool runRestoredPathSmoke(lely::canopen::AsyncMaster& master,
                          const EmcySavedState& state)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(kHeartbeatSettleMs));

    CanopenEmcyEvent fault;
    if (!triggerHeartbeatFault(master, fault)) {
        return false;
    }
    bool result = validateHeartbeatEmcyData(
        fault, kHeartbeatConsumerEmcyCode, "A06 restored-path fault");

    /* Restoring 0x1015 also restores its timing contract. Allow the reset
     * wait to cover the saved inhibit interval plus one Producer Heartbeat and
     * host/RTOS scheduling margin instead of assuming the 5 s test timeout. */
    const std::uint32_t saved_inhibit_ms =
        (static_cast<std::uint32_t>(state.slave_inhibit_time) + 9U) / 10U;
    const std::uint32_t recovery_timeout_ms = std::max<std::uint32_t>(
        kEmcyWaitTimeoutMs, saved_inhibit_ms
                                + static_cast<std::uint32_t>(
                                    state.master_heartbeat)
                                + kInhibitUpperMarginMs);

    CanopenEmcyEvent reset;
    if (!recoverHeartbeatFault(
            master, state.master_heartbeat,
            std::chrono::milliseconds(recovery_timeout_ms), reset)) {
        return false;
    }
    if (!validateHeartbeatEmcyData(reset, kEmcyResetCode,
                                   "A06 restored-path recovery")
        || !validateErrorRegister(master, reset, false,
                                  "A06 restored-path recovery")) {
        result = false;
    }

    std::uint32_t device_type = 0;
    if (!readRemoteObject(master, 0x1000, kScalarSubindex, device_type,
                          "Device Type smoke read")) {
        result = false;
    } else {
        spdlog::info("A06 restored-path SDO smoke read: 0x1000=0x{:08x}",
                     device_type);
    }
    return result;
}

} // namespace

int emcyProcess(lely::canopen::AsyncMaster& master)
{
    EmcySavedState state;
    if (!captureSavedState(master, state)) {
        return 1;
    }

    ErrorHistorySnapshot initial;
    if (!readErrorHistory(master, initial)) {
        return 1;
    }
    if (initial.error_register != 0U) {
        spdlog::error(
            "A06 refuses fault injection while 0x1001 is already non-zero: "
            "0x{:02x}",
            static_cast<unsigned int>(initial.error_register));
        return 1;
    }

    int result = 0;

    if (!runDefaultEmcyValidation(master, state)) {
        result = 1;
    }

    if (result == 0 && !configureTestEmcyCobId(master)) {
        result = 1;
    }

    if (result == 0 && !runConfigurableInhibitValidation(master, state)) {
        result = 1;
    }

    /* Restore before the final smoke test. Failure here means the environment
     * is already untrusted, so do not continue generating new test faults. */
    if (!restoreEmcyConfiguration(master, state)) {
        result = 1;
    } else if (result == 0 && !runRestoredPathSmoke(master, state)) {
        result = 1;
    }

    /* Run the same idempotent restoration once more because the smoke test can
     * fail after stopping heartbeat. This is the final cleanup boundary. */
    if (!restoreEmcyConfiguration(master, state)) {
        result = 1;
    }

    if (result == 0) {
        spdlog::info(
            "A06 EMCY producer, history, configurable COB-ID, inhibit, and "
            "restoration passed");
    }
    return result;
}
