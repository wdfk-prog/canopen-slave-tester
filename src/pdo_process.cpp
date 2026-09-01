/**
 * @file
 * @brief Implements RPDO and TPDO validation.
 */

#include "pdo_process.h"

#include "canopen_config.h"
#include "canopen_nmt.h"
#include "canopen_sdo.h"

#include <lely/coapp/master.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <system_error>

namespace {

/** PDO number 1 is the only PDO exercised by the current demo OD. */
constexpr int kPdoNumber = 1;

/** First TPDO1-mapped UNSIGNED32 object. */
constexpr std::uint16_t kTpdoValue1Index = 0x2100;
/** 0x2100 is a scalar demo object, therefore sub-index zero is used. */
constexpr std::uint8_t kTpdoValue1Subindex = 0x00;
/** Second TPDO1-mapped UNSIGNED32 object. */
constexpr std::uint16_t kTpdoValue2Index = 0x2101;
/** 0x2101 is a scalar demo object, therefore sub-index zero is used. */
constexpr std::uint8_t kTpdoValue2Subindex = 0x00;
/** RPDO1-mapped writable demo object used for delivery verification. */
constexpr std::uint16_t kRpdoTestIndex = 0x2200;
/** 0x2200 is scalar in the current demo OD. */
constexpr std::uint8_t kRpdoTestSubindex = 0x00;

/** Primary RPDO probe pattern with alternating nibbles for easy diagnosis. */
constexpr std::uint32_t kRpdoProbeValue = 0xA5A5A5A5;
/** Alternate probe guarantees the transmitted value differs from the saved
 * value when the object already contains the primary pattern. */
constexpr std::uint32_t kRpdoAlternateValue = 0x5A5A5A5A;
/** Five TPDO frames provide four timing intervals for periodicity checking. */
constexpr std::size_t kTpdoSampleCount = 5;
/** Default demo TPDO1 event timer configured by the generated DCF. */
constexpr std::uint32_t kTpdoPeriodMs = 1000;
/** Allows host scheduling and CAN arbitration jitter without masking large
 * TPDO period errors. */
constexpr std::uint32_t kTpdoToleranceMs = 150;
/** Collection timeout includes two extra event periods so startup phase and
 * scheduling jitter do not create false failures. */
constexpr std::uint32_t kTpdoCollectionTimeoutMs =
    (static_cast<std::uint32_t>(kTpdoSampleCount) + 2U) * kTpdoPeriodMs;
/** RPDO send callback should complete well inside two seconds on this bus. */
constexpr std::uint32_t kRpdoTransmitTimeoutMs = 2000;
/** Retry a TPDO/OD consistency snapshot when a new TPDO races the SDO reads. */
constexpr unsigned int kConsistencyRetryCount = 3;
/** TPDO1 maps two UNSIGNED32 values and therefore carries eight bytes. */
constexpr std::size_t kTpdoPayloadLength = 8;
/** RPDO1 maps one UNSIGNED32 value and therefore carries four bytes. */
constexpr std::size_t kRpdoPayloadLength = 4;

/** Monotonic clock used for interval measurements unaffected by wall time. */
using Clock = std::chrono::steady_clock;
/** Lely PDO callback signature used when unregistering PDO validation callbacks. */
using PdoCallback =
    std::function<void(int, std::error_code, const void*, std::size_t)>;

/** One validated TPDO1 observation captured by the receive callback. */
struct TpdoSample {
    std::array<std::uint8_t, kTpdoPayloadLength> payload{}; /**< Zero-filled copy of payload bytes. */
    std::size_t length = 0; /**< Zero until callback stores the received DLC. */
    Clock::time_point timestamp{}; /**< Monotonic receive time; epoch default until filled. */
};

/** Shared TPDO receive state written by the event loop and read by PDO validation. */
struct TpdoReceiveState {
    std::mutex mutex; /**< Protects all callback-published fields. */
    std::condition_variable condition; /**< Wakes waits on sample/failure changes. */
    std::array<TpdoSample, kTpdoSampleCount> samples{}; /**< First sample window, zero-initialized. */
    std::size_t sample_count = 0; /**< Number of valid entries stored in samples. */
    std::size_t generation = 0; /**< Monotonic counter incremented for every valid TPDO1. */
    TpdoSample latest{}; /**< Most recent valid frame for OD consistency checks. */
    bool failed = false; /**< false until callback detects invalid input/error. */
    bool null_payload = false; /**< Records nonzero-length callback with null data. */
    std::size_t invalid_length = 0; /**< Zero before failure; stores callback-reported DLC on invalid TPDO input. */
    std::error_code error; /**< Default success; callback stores Lely processing error. */
};

/** Shared RPDO transmit-completion state published by Lely OnTpdo(). */
struct RpdoTransmitState {
    std::mutex mutex; /**< Protects completion metadata and payload copy. */
    std::condition_variable condition; /**< Wakes the RPDO send waiter. */
    bool completed = false; /**< false until the corresponding send callback runs. */
    int pdo_number = 0; /**< Zero sentinel until callback reports a PDO number. */
    std::error_code error; /**< Default success; callback stores send error. */
    std::array<std::uint8_t, kTpdoPayloadLength> payload{}; /**< Zero-filled callback payload copy. */
    std::size_t length = 0; /**< Zero until callback reports transmitted DLC. */
};

/**
 * @brief Decode an UNSIGNED32 value from CANopen little-endian bytes.
 *
 * @param data Pointer to at least four payload bytes.
 * @return Decoded 32-bit value.
 */
std::uint32_t decodeLe32(const std::uint8_t* data) noexcept
{
    return static_cast<std::uint32_t>(data[0])
           | (static_cast<std::uint32_t>(data[1]) << 8U)
           | (static_cast<std::uint32_t>(data[2]) << 16U)
           | (static_cast<std::uint32_t>(data[3]) << 24U);
}

/**
 * @brief Register the PDO callbacks used by PDO validation.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param receive_state Shared TPDO receive state.
 * @param transmit_state Shared RPDO transmit state.
 */
void registerPdoCallbacks(
    lely::canopen::AsyncMaster& master,
    const std::shared_ptr<TpdoReceiveState>& receive_state,
    const std::shared_ptr<RpdoTransmitState>& transmit_state)
{
    master.OnRpdo(
        [receive_state](int num, std::error_code error, const void* payload,
                        std::size_t length) noexcept {
            if (num != kPdoNumber) {
                return;
            }

            /* sample starts zeroed so short/invalid payloads never expose
             * uninitialized bytes in diagnostics. */
            TpdoSample sample;
            sample.length = length;
            sample.timestamp = Clock::now();
            if (payload != nullptr && length != 0U) {
                /* Clamp copy length to local storage even when Lely reports an
                 * invalid oversized DLC; validation below still marks failure. */
                const std::size_t copy_length =
                    std::min(length, sample.payload.size());
                std::memcpy(sample.payload.data(), payload, copy_length);
            }

            {
                /* Publish sample/error state atomically to the process thread. */
                std::lock_guard<std::mutex> lock(receive_state->mutex);
                if (error || length != kTpdoPayloadLength
                    || (payload == nullptr && length != 0U)) {
                    receive_state->failed = true;
                    receive_state->error = error;
                    receive_state->invalid_length = length;
                    receive_state->null_payload =
                        payload == nullptr && length != 0U;
                } else {
                    receive_state->latest = sample;
                    ++receive_state->generation;
                    if (receive_state->sample_count
                        < receive_state->samples.size()) {
                        receive_state->samples[receive_state->sample_count] =
                            sample;
                        ++receive_state->sample_count;
                    }
                }
            }
            receive_state->condition.notify_all();
        });

    master.OnTpdo(
        [transmit_state](int num, std::error_code error, const void* payload,
                         std::size_t length) noexcept {
            if (num != kPdoNumber) {
                return;
            }

            /* Serialize the entire transmit callback snapshot so the waiter
             * sees metadata and payload from the same completion event. */
            std::lock_guard<std::mutex> lock(transmit_state->mutex);
            transmit_state->completed = true;
            transmit_state->pdo_number = num;
            transmit_state->error = error;
            transmit_state->length = length;
            transmit_state->payload.fill(0);
            if (payload != nullptr && length != 0U) {
                /* Clamp copied bytes to the fixed diagnostic buffer. */
                const std::size_t copy_length =
                    std::min(length, transmit_state->payload.size());
                std::memcpy(transmit_state->payload.data(), payload,
                            copy_length);
            }
            transmit_state->condition.notify_all();
        });
}

/**
 * @brief Remove the PDO callbacks installed by PDO validation.
 *
 * @param master Active Lely asynchronous CANopen master.
 */
void clearPdoCallbacks(lely::canopen::AsyncMaster& master)
{
    master.OnRpdo(PdoCallback{});
    master.OnTpdo(PdoCallback{});
}

/**
 * @brief Wait for the configured TPDO sample set.
 *
 * @param state Shared receive state populated by the event-loop thread.
 * @return true when all required valid samples were received; otherwise false.
 */
bool waitForTpdoSamples(const std::shared_ptr<TpdoReceiveState>& state)
{
    /* unique_lock lets the callback acquire the mutex while this thread is
     * sleeping inside condition_variable::wait_for(). */
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(
            lock, std::chrono::milliseconds(kTpdoCollectionTimeoutMs),
            [state]() {
                return state->failed
                       || state->sample_count >= kTpdoSampleCount;
            })) {
        spdlog::error("PDO validation TPDO1 sampling timed out: received={}/{}",
                      state->sample_count, kTpdoSampleCount);
        return false;
    }

    if (state->failed) {
        if (state->error) {
            spdlog::error("PDO validation TPDO1 processing failed: {}",
                          state->error.message());
        } else if (state->null_payload) {
            spdlog::error("PDO validation TPDO1 callback returned a null payload");
        } else {
            spdlog::error("PDO validation TPDO1 length mismatch: expected={} actual={}",
                          kTpdoPayloadLength, state->invalid_length);
        }
        return false;
    }

    return true;
}

/**
 * @brief Validate TPDO1 timing and log decoded sample values.
 *
 * @param state Shared receive state containing the first sample set.
 * @return true when every interval is within tolerance; otherwise false.
 */
bool validateTpdoSamples(const std::shared_ptr<TpdoReceiveState>& state)
{
    /* Copy the callback-owned sample window once so timing validation runs
     * without holding the receive mutex or blocking later TPDO callbacks. */
    std::array<TpdoSample, kTpdoSampleCount> samples;
    {
        /* Protect the snapshot copy from concurrent callback updates. */
        std::lock_guard<std::mutex> lock(state->mutex);
        samples = state->samples;
    }

    /* Inclusive lower bound derived from the configured period tolerance. */
    const std::int64_t minimum_interval_ms =
        static_cast<std::int64_t>(kTpdoPeriodMs - kTpdoToleranceMs);
    /* Inclusive upper bound derived from the configured period tolerance. */
    const std::int64_t maximum_interval_ms =
        static_cast<std::int64_t>(kTpdoPeriodMs + kTpdoToleranceMs);

    /* i identifies each captured frame and, from i=1 onward, its preceding
     * inter-frame timing interval. */
    for (std::size_t i = 0; i < samples.size(); ++i) {
        /* TPDO1 bytes 0..3 map 0x2100:00 in little-endian CANopen order. */
        const std::uint32_t value1 = decodeLe32(samples[i].payload.data());
        /* TPDO1 bytes 4..7 map 0x2101:00. */
        const std::uint32_t value2 =
            decodeLe32(samples[i].payload.data() + 4U);
        /* Offset is relative to sample zero to make logs independent of the
         * arbitrary steady_clock epoch. */
        const auto offset =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                samples[i].timestamp - samples[0].timestamp);
        spdlog::info(
            "PDO validation TPDO1 sample[{}] t=+{} ms: 0x2100:00=0x{:08x} "
            "0x2101:00=0x{:08x}",
            i, offset.count(), value1, value2);

        if (i == 0U) {
            continue;
        }

        /* Compare only adjacent samples because the PDO requirement is a
         * per-period bound, not cumulative drift from sample zero. */
        const auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(
            samples[i].timestamp - samples[i - 1U].timestamp);
        /* Convert to signed arithmetic so bounds remain valid if configuration
         * changes toward smaller periods/tolerances in the future. */
        const std::int64_t interval_ms = interval.count();
        spdlog::info("PDO validation TPDO1 interval[{}]={} ms", i, interval_ms);
        if (interval_ms < minimum_interval_ms
            || interval_ms > maximum_interval_ms) {
            spdlog::error(
                "PDO validation TPDO1 period out of tolerance: expected={}+/-{} ms "
                "actual={} ms",
                kTpdoPeriodMs, kTpdoToleranceMs, interval_ms);
            return false;
        }
    }

    return true;
}

/**
 * @brief Read a TPDO-mapped remote UNSIGNED32 value from the local mapping.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param index Remote object index.
 * @param subindex Remote object sub-index.
 * @param value Receives the mapped value on success.
 * @return true on success; otherwise false.
 */
bool readMappedTpdoValue(lely::canopen::AsyncMaster& master,
                         std::uint16_t index, std::uint8_t subindex,
                         std::uint32_t& value)
{
    /* Local mapped-object reads are synchronous and do not issue remote SDO;
     * default construction represents no local mapping error. */
    std::error_code error;
    value = master.RpdoMapped(CANOPEN_SLAVE_NODE_ID)[index][subindex]
                .Read<std::uint32_t>(error);
    if (error) {
        spdlog::error(
            "PDO validation unable to read TPDO-mapped object 0x{:04x}:{:02x}: {}",
            index, static_cast<unsigned int>(subindex), error.message());
        return false;
    }
    return true;
}

/**
 * @brief Obtain the latest valid TPDO sample and receive generation.
 *
 * @param state Shared TPDO receive state.
 * @param sample Receives the latest valid sample.
 * @param generation Receives the sample generation.
 * @return true when a valid sample is available and no receive error exists.
 */
bool getLatestTpdoSample(const std::shared_ptr<TpdoReceiveState>& state,
                         TpdoSample& sample, std::size_t& generation)
{
    /* Copy latest frame and generation as one coherent callback snapshot. */
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->failed || state->generation == 0U) {
        return false;
    }
    sample = state->latest;
    generation = state->generation;
    return true;
}

/**
 * @brief Check whether the TPDO receive generation is still unchanged.
 *
 * @param state Shared TPDO receive state.
 * @param generation Generation captured before an OD comparison.
 * @return true when no newer TPDO arrived and no receive error occurred.
 */
bool isTpdoGenerationStable(const std::shared_ptr<TpdoReceiveState>& state,
                            std::size_t generation)
{
    /* Compare against the caller's captured generation under the same mutex
     * used by the receive callback. */
    std::lock_guard<std::mutex> lock(state->mutex);
    return !state->failed && state->generation == generation;
}

/**
 * @brief Verify TPDO raw bytes against Lely mapping and remote OD values.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param state Shared TPDO receive state.
 * @param completion_wait_timed_out Set when a local SDO completion wait times
 * out and no further SDO operation should be attempted safely.
 * @return true on success; otherwise false.
 */
bool verifyTpdoOdConsistency(
    lely::canopen::AsyncMaster& master,
    const std::shared_ptr<TpdoReceiveState>& state,
    bool& completion_wait_timed_out)
{
    /* attempt bounds retries when a periodic TPDO lands between the raw-frame,
     * mapped-value, and SDO snapshots. */
    for (unsigned int attempt = 0; attempt < kConsistencyRetryCount;
         ++attempt) {
        /* Default sample is empty until getLatestTpdoSample() replaces it. */
        TpdoSample sample;
        /* Zero means no TPDO generation has been captured yet. */
        std::size_t generation = 0;
        if (!getLatestTpdoSample(state, sample, generation)) {
            spdlog::error("PDO validation TPDO1 has no valid sample for OD comparison");
            return false;
        }

        /* Decode the first mapped value directly from the captured CAN bytes. */
        const std::uint32_t raw_value1 = decodeLe32(sample.payload.data());
        /* Decode the second mapped value from bytes 4..7. */
        const std::uint32_t raw_value2 =
            decodeLe32(sample.payload.data() + 4U);

        /* Zero is only a placeholder until the 0x2100 local mapping read succeeds. */
        std::uint32_t mapped_value1 = 0;
        /* Zero is only a placeholder until the 0x2101 local mapping read succeeds. */
        std::uint32_t mapped_value2 = 0;
        if (!readMappedTpdoValue(master, kTpdoValue1Index,
                                 kTpdoValue1Subindex, mapped_value1)
            || !readMappedTpdoValue(master, kTpdoValue2Index,
                                    kTpdoValue2Subindex, mapped_value2)) {
            return false;
        }
        if (!isTpdoGenerationStable(state, generation)) {
            continue;
        }

        /* Remote SDO values provide an independent OD view of TPDO content. */
        std::uint32_t sdo_value1 = 0;
        /* Preserve the first upload result so WAIT_TIMEOUT can stop later SDO. */
        const SdoOperationResult read1 = readRemoteSdo<std::uint32_t>(
            master, CANOPEN_SLAVE_NODE_ID, kTpdoValue1Index,
            kTpdoValue1Subindex, sdo_value1);
        if (read1 == SdoOperationResult::WAIT_TIMEOUT) {
            completion_wait_timed_out = true;
            return false;
        }
        if (read1 != SdoOperationResult::SUCCESS) {
            return false;
        }

        /* Zero placeholder for the second remote OD value. */
        std::uint32_t sdo_value2 = 0;
        /* Keep the second upload result separate for precise failure handling. */
        const SdoOperationResult read2 = readRemoteSdo<std::uint32_t>(
            master, CANOPEN_SLAVE_NODE_ID, kTpdoValue2Index,
            kTpdoValue2Subindex, sdo_value2);
        if (read2 == SdoOperationResult::WAIT_TIMEOUT) {
            completion_wait_timed_out = true;
            return false;
        }
        if (read2 != SdoOperationResult::SUCCESS) {
            return false;
        }

        if (!isTpdoGenerationStable(state, generation)) {
            spdlog::info(
                "PDO validation TPDO1 changed during OD comparison; retrying ({}/{})",
                attempt + 1U, kConsistencyRetryCount);
            continue;
        }

        if (mapped_value1 != raw_value1 || mapped_value2 != raw_value2) {
            spdlog::error(
                "PDO validation TPDO1 mapped value mismatch: raw=(0x{:08x},0x{:08x}) "
                "mapped=(0x{:08x},0x{:08x})",
                raw_value1, raw_value2, mapped_value1, mapped_value2);
            return false;
        }
        if (sdo_value1 != raw_value1 || sdo_value2 != raw_value2) {
            spdlog::error(
                "PDO validation TPDO1/OD mismatch: TPDO=(0x{:08x},0x{:08x}) "
                "SDO=(0x{:08x},0x{:08x})",
                raw_value1, raw_value2, sdo_value1, sdo_value2);
            return false;
        }

        spdlog::info(
            "PDO validation TPDO1 mapping/OD verified: 0x2100:00=0x{:08x} "
            "0x2101:00=0x{:08x}",
            raw_value1, raw_value2);
        return true;
    }

    spdlog::error(
        "PDO validation TPDO1 changed during all {} OD comparison attempts",
        kConsistencyRetryCount);
    return false;
}

/**
 * @brief Send a value to the slave RPDO1 through the Lely PDO API.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param state Shared transmit callback state.
 * @param value Probe or restoration value to transmit.
 * @return true when the master TPDO1 for the slave RPDO1 was sent with
 * the expected payload; otherwise false.
 */
bool sendRpdoValue(lely::canopen::AsyncMaster& master,
                   const std::shared_ptr<RpdoTransmitState>& state,
                   std::uint32_t value)
{
    {
        /* Clear the previous send result before triggering a new PDO event so
         * stale callback data cannot satisfy this transmission wait. */
        std::lock_guard<std::mutex> lock(state->mutex);
        state->completed = false;
        state->pdo_number = 0;
        state->error.clear();
        state->payload.fill(0);
        state->length = 0;
    }

    /* Local mapping operations report synchronously through error, which
     * starts in the no-error state. */
    std::error_code error;
    /* This mapped proxy targets the master's local TPDO representation for the
     * slave RPDO object 0x2200:00; WriteEvent() transmits it on CAN. */
    auto mapped = master.TpdoMapped(CANOPEN_SLAVE_NODE_ID)[kRpdoTestIndex]
                                    [kRpdoTestSubindex];
    mapped.Write(value, error);
    if (error) {
        spdlog::error("PDO validation unable to write RPDO-mapped 0x2200:00: {}",
                      error.message());
        return false;
    }
    mapped.WriteEvent(error);
    if (error) {
        spdlog::error("PDO validation unable to trigger RPDO1 transmission: {}",
                      error.message());
        return false;
    }

    /* Wait for Lely's TPDO send callback while the event-loop thread remains
     * free to perform the actual CAN transmission. */
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(
            lock, std::chrono::milliseconds(kRpdoTransmitTimeoutMs),
            [state]() { return state->completed; })) {
        spdlog::error("PDO validation RPDO1 transmission callback timed out");
        return false;
    }
    if (state->error) {
        spdlog::error("PDO validation RPDO1 transmission failed: {}",
                      state->error.message());
        return false;
    }
    if (state->pdo_number != kPdoNumber
        || state->length != kRpdoPayloadLength) {
        spdlog::error(
            "PDO validation RPDO1 transmission mismatch: pdo={} expected_dlc={} "
            "actual_dlc={}",
            state->pdo_number, kRpdoPayloadLength, state->length);
        return false;
    }

    /* Decode the callback payload to prove the transmitted RPDO value matches
     * the requested probe rather than merely receiving a success callback. */
    const std::uint32_t payload_value = decodeLe32(state->payload.data());
    if (payload_value != value) {
        spdlog::error(
            "PDO validation RPDO1 payload mismatch: expected=0x{:08x} actual=0x{:08x}",
            value, payload_value);
        return false;
    }

    spdlog::info("PDO validation RPDO1 transmitted: 0x2200:00=0x{:08x}", value);
    return true;
}

/**
 * @brief Restore 0x2200:00 through SDO and verify it by read-back.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param original_value Saved value to restore.
 * @param completion_wait_timed_out Set when local SDO completion becomes
 * unknown and later SDO transactions should not be submitted.
 * @return true when restoration is confirmed; otherwise false.
 */
bool restoreRpdoTestValue(lely::canopen::AsyncMaster& master,
                          std::uint32_t original_value,
                          bool& completion_wait_timed_out)
{
    /* Restore through SDO because read-back verification needs a known remote
     * OD state independent of the PDO transmission path under test. */
    const SdoOperationResult write_result = writeRemoteSdo<std::uint32_t>(
        master, CANOPEN_SLAVE_NODE_ID, kRpdoTestIndex,
        kRpdoTestSubindex, original_value);
    if (write_result == SdoOperationResult::WAIT_TIMEOUT) {
        completion_wait_timed_out = true;
        return false;
    }
    if (write_result == SdoOperationResult::FAILED) {
        return false;
    }
    if (write_result == SdoOperationResult::SDO_TIMEOUT) {
        spdlog::warn(
            "PDO validation restore SDO response timed out; verifying 0x2200:00 by "
            "read-back");
    }

    /* Zero is a neutral placeholder until the verification upload completes. */
    std::uint32_t restored_value = 0;
    /* Preserve read result classification for WAIT_TIMEOUT safety handling. */
    const SdoOperationResult read_result = readRemoteSdo<std::uint32_t>(
        master, CANOPEN_SLAVE_NODE_ID, kRpdoTestIndex,
        kRpdoTestSubindex, restored_value);
    if (read_result == SdoOperationResult::WAIT_TIMEOUT) {
        completion_wait_timed_out = true;
        return false;
    }
    if (read_result != SdoOperationResult::SUCCESS) {
        return false;
    }
    if (restored_value != original_value) {
        spdlog::error(
            "PDO validation restored 0x2200:00 mismatch: expected=0x{:08x} "
            "actual=0x{:08x}",
            original_value, restored_value);
        return false;
    }

    spdlog::info("PDO validation original 0x2200:00 restored: 0x{:08x}",
                 restored_value);
    return true;
}

} // namespace

int pdoProcess(lely::canopen::AsyncMaster& master)
{
    /* Shared receive state starts empty because PDO validation must collect a fresh TPDO
     * sample set after the callbacks are installed. */
    const auto receive_state = std::make_shared<TpdoReceiveState>();
    /* Shared transmit state starts incomplete because no RPDO probe has been
     * requested yet. */
    const auto transmit_state = std::make_shared<RpdoTransmitState>();
    registerPdoCallbacks(master, receive_state, transmit_state);

    /* result remains zero until any protocol assertion or cleanup step fails. */
    int result = 0;
    /* false prevents restoration until the original 0x2200:00 value is known. */
    bool original_value_saved = false;
    /* false means the current SDO channel completion state is known; once set,
     * no additional SDO cleanup is submitted because the prior callback may
     * still complete later. */
    bool completion_wait_timed_out = false;
    /* Neutral storage overwritten by the initial 0x2200:00 SDO upload. */
    std::uint32_t original_value = 0;

    /* Step 1: keep the local master Operational so its PDO services remain
     * active while PDO validation collects and transmits PDO traffic. */
    if (!issueNmtCommand(master, lely::canopen::NmtCommand::START,
                         CANOPEN_MASTER_NODE_ID,
                         "PDO validation local master NMT Start")) {
        result = 1;
    }

    /* Step 2: put the slave in Operational, which enables its configured PDO
     * production and consumption. */
    if (result == 0
        && !issueNmtCommand(master, lely::canopen::NmtCommand::START,
                            CANOPEN_SLAVE_NODE_ID,
                            "PDO validation slave NMT Start")) {
        result = 1;
    }
    /* Step 3: TPDO1 is event-driven (transmission type 254) with a 1000 ms
     * event timer. Collect five frames, validate timing, then compare raw CAN
     * bytes with both Lely's mapped view and independent remote SDO reads. */
    if (result == 0 && !waitForTpdoSamples(receive_state)) {
        result = 1;
    }
    if (result == 0 && !validateTpdoSamples(receive_state)) {
        result = 1;
    }
    if (result == 0
        && !verifyTpdoOdConsistency(master, receive_state,
                                    completion_wait_timed_out)) {
        result = 1;
    }

    /* Step 4: save the RPDO-mapped user object before transmitting a probe so
     * the test can restore the exact pre-test value. */
    if (result == 0) {
        /* Preserve the detailed SDO result to detect an unknown callback state. */
        const SdoOperationResult read_result = readRemoteSdo<std::uint32_t>(
            master, CANOPEN_SLAVE_NODE_ID, kRpdoTestIndex,
            kRpdoTestSubindex, original_value);
        if (read_result == SdoOperationResult::WAIT_TIMEOUT) {
            completion_wait_timed_out = true;
            result = 1;
        } else if (read_result != SdoOperationResult::SUCCESS) {
            result = 1;
        } else {
            original_value_saved = true;
            spdlog::info("PDO validation saved original 0x2200:00=0x{:08x}",
                         original_value);
        }
    }

    /* Step 5: choose a probe guaranteed to differ from the saved value; this
     * avoids a false PASS where the OD already contained the nominal pattern. */
    const std::uint32_t probe_value =
        original_value == kRpdoProbeValue ? kRpdoAlternateValue
                                           : kRpdoProbeValue;
    if (result == 0) {
        if (!sendRpdoValue(master, transmit_state, probe_value)) {
            result = 1;
        }
    }

    /* Step 6: independently confirm the RPDO write reached the remote OD via
     * SDO read-back, not only via the local transmit callback. */
    if (result == 0) {
        /* Zero is only a placeholder until the SDO upload succeeds. */
        std::uint32_t read_back_value = 0;
        /* Exact completion class controls whether later cleanup is safe. */
        const SdoOperationResult read_result = readRemoteSdo<std::uint32_t>(
            master, CANOPEN_SLAVE_NODE_ID, kRpdoTestIndex,
            kRpdoTestSubindex, read_back_value);
        if (read_result == SdoOperationResult::WAIT_TIMEOUT) {
            completion_wait_timed_out = true;
            result = 1;
        } else if (read_result != SdoOperationResult::SUCCESS) {
            result = 1;
        } else if (read_back_value != probe_value) {
            spdlog::error(
                "PDO validation RPDO1/OD mismatch: expected=0x{:08x} actual=0x{:08x}",
                probe_value, read_back_value);
            result = 1;
        } else {
            spdlog::info("PDO validation RPDO1 OD update verified: 0x2200:00=0x{:08x}",
                         read_back_value);
        }
    }

    /* Step 7: restore 0x2200:00 whenever its original value was captured. A
     * local completion wait timeout forces best-effort PDO restoration because
     * another SDO transaction could overlap an unknown in-flight request. */
    if (original_value_saved) {
        if (!completion_wait_timed_out
            && !restoreRpdoTestValue(master, original_value,
                                     completion_wait_timed_out)) {
            spdlog::error("PDO validation 0x2200:00 restoration was not verified");
            result = 1;
        }

        if (completion_wait_timed_out) {
            spdlog::warn(
                "PDO validation local SDO completion state is unknown; attempting "
                "best-effort RPDO restoration without SDO verification");
            if (!sendRpdoValue(master, transmit_state, original_value)) {
                spdlog::error("PDO validation best-effort RPDO restoration failed");
            }
            result = 1;
        }
    }

    /* Step 8: always unregister PDO validation callbacks so later processes do not consume
     * stale TPDO/RPDO events through PDO validation state objects. */
    clearPdoCallbacks(master);

    if (result == 0) {
        spdlog::info("PDO validation RPDO/TPDO test passed");
    }
    return result;
}
