/**
 * @file
 * @brief Implements RPDO and TPDO validation.
 */

#include "pdo_process.h"

#include "canopen_config.h"

#include <lely/coapp/master.hpp>
#include <lely/coapp/sdo_error.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <system_error>

namespace {

constexpr int kPdoNumber = 1;

constexpr std::uint16_t kTpdoValue1Index = 0x2100;
constexpr std::uint8_t kTpdoValue1Subindex = 0x00;
constexpr std::uint16_t kTpdoValue2Index = 0x2101;
constexpr std::uint8_t kTpdoValue2Subindex = 0x00;
constexpr std::uint16_t kRpdoTestIndex = 0x2200;
constexpr std::uint8_t kRpdoTestSubindex = 0x00;

constexpr std::uint32_t kRpdoProbeValue = 0xA5A5A5A5;
constexpr std::uint32_t kRpdoAlternateValue = 0x5A5A5A5A;
constexpr std::size_t kTpdoSampleCount = 5;
constexpr std::uint32_t kTpdoPeriodMs = 1000;
constexpr std::uint32_t kTpdoToleranceMs = 150;
constexpr std::uint32_t kTpdoCollectionTimeoutMs =
    (static_cast<std::uint32_t>(kTpdoSampleCount) + 2U) * kTpdoPeriodMs;
constexpr std::uint32_t kRpdoTransmitTimeoutMs = 2000;
constexpr std::uint32_t kSdoTimeoutMs = 5000;
constexpr std::uint32_t kSdoCompletionMarginMs = 500;
constexpr unsigned int kConsistencyRetryCount = 3;
constexpr std::size_t kTpdoPayloadLength = 8;
constexpr std::size_t kRpdoPayloadLength = 4;

using Clock = std::chrono::steady_clock;
using PdoCallback =
    std::function<void(int, std::error_code, const void*, std::size_t)>;

enum class SdoOperationResult {
    SUCCESS,
    FAILED,
    SDO_TIMEOUT,
    WAIT_TIMEOUT,
};

struct TpdoSample {
    std::array<std::uint8_t, kTpdoPayloadLength> payload{};
    std::size_t length = 0;
    Clock::time_point timestamp{};
};

struct TpdoReceiveState {
    std::mutex mutex;
    std::condition_variable condition;
    std::array<TpdoSample, kTpdoSampleCount> samples{};
    std::size_t sample_count = 0;
    std::size_t generation = 0;
    TpdoSample latest{};
    bool failed = false;
    bool null_payload = false;
    std::size_t invalid_length = 0;
    std::error_code error;
};

struct RpdoTransmitState {
    std::mutex mutex;
    std::condition_variable condition;
    bool completed = false;
    int pdo_number = 0;
    std::error_code error;
    std::array<std::uint8_t, kTpdoPayloadLength> payload{};
    std::size_t length = 0;
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
 * @brief Register the PDO callbacks used by A03.
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

            TpdoSample sample;
            sample.length = length;
            sample.timestamp = Clock::now();
            if (payload != nullptr && length != 0U) {
                const std::size_t copy_length =
                    std::min(length, sample.payload.size());
                std::memcpy(sample.payload.data(), payload, copy_length);
            }

            {
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

            std::lock_guard<std::mutex> lock(transmit_state->mutex);
            transmit_state->completed = true;
            transmit_state->pdo_number = num;
            transmit_state->error = error;
            transmit_state->length = length;
            transmit_state->payload.fill(0);
            if (payload != nullptr && length != 0U) {
                const std::size_t copy_length =
                    std::min(length, transmit_state->payload.size());
                std::memcpy(transmit_state->payload.data(), payload,
                            copy_length);
            }
            transmit_state->condition.notify_all();
        });
}

/**
 * @brief Remove the PDO callbacks installed by A03.
 *
 * @param master Active Lely asynchronous CANopen master.
 */
void clearPdoCallbacks(lely::canopen::AsyncMaster& master)
{
    master.OnRpdo(PdoCallback{});
    master.OnTpdo(PdoCallback{});
}

/**
 * @brief Issue an NMT command and convert exceptions into a process failure.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param command NMT command to issue.
 * @param node_id Target node-ID.
 * @param description Text used in failure diagnostics.
 * @return true on success; otherwise false.
 */
bool issueNmtCommand(lely::canopen::AsyncMaster& master,
                     lely::canopen::NmtCommand command,
                     std::uint8_t node_id, const char* description)
{
    try {
        master.Command(command, node_id);
    } catch (const std::exception& exception) {
        spdlog::error("{} failed: {}", description, exception.what());
        return false;
    }
    return true;
}

/**
 * @brief Wait for the configured TPDO sample set.
 *
 * @param state Shared receive state populated by the event-loop thread.
 * @return true when all required valid samples were received; otherwise false.
 */
bool waitForTpdoSamples(const std::shared_ptr<TpdoReceiveState>& state)
{
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(
            lock, std::chrono::milliseconds(kTpdoCollectionTimeoutMs),
            [state]() {
                return state->failed
                       || state->sample_count >= kTpdoSampleCount;
            })) {
        spdlog::error("A03 TPDO1 sampling timed out: received={}/{}",
                      state->sample_count, kTpdoSampleCount);
        return false;
    }

    if (state->failed) {
        if (state->error) {
            spdlog::error("A03 TPDO1 processing failed: {}",
                          state->error.message());
        } else if (state->null_payload) {
            spdlog::error("A03 TPDO1 callback returned a null payload");
        } else {
            spdlog::error("A03 TPDO1 length mismatch: expected={} actual={}",
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
    std::array<TpdoSample, kTpdoSampleCount> samples;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        samples = state->samples;
    }

    const std::int64_t minimum_interval_ms =
        static_cast<std::int64_t>(kTpdoPeriodMs - kTpdoToleranceMs);
    const std::int64_t maximum_interval_ms =
        static_cast<std::int64_t>(kTpdoPeriodMs + kTpdoToleranceMs);

    for (std::size_t i = 0; i < samples.size(); ++i) {
        const std::uint32_t value1 = decodeLe32(samples[i].payload.data());
        const std::uint32_t value2 =
            decodeLe32(samples[i].payload.data() + 4U);
        const auto offset =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                samples[i].timestamp - samples[0].timestamp);
        spdlog::info(
            "A03 TPDO1 sample[{}] t=+{} ms: 0x2100:00=0x{:08x} "
            "0x2101:00=0x{:08x}",
            i, offset.count(), value1, value2);

        if (i == 0U) {
            continue;
        }

        const auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(
            samples[i].timestamp - samples[i - 1U].timestamp);
        const std::int64_t interval_ms = interval.count();
        spdlog::info("A03 TPDO1 interval[{}]={} ms", i, interval_ms);
        if (interval_ms < minimum_interval_ms
            || interval_ms > maximum_interval_ms) {
            spdlog::error(
                "A03 TPDO1 period out of tolerance: expected={}+/-{} ms "
                "actual={} ms",
                kTpdoPeriodMs, kTpdoToleranceMs, interval_ms);
            return false;
        }
    }

    return true;
}

/**
 * @brief Read an UNSIGNED32 object from the remote node through SDO.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param index Remote object index.
 * @param subindex Remote object sub-index.
 * @param value Receives the uploaded value on success.
 * @return Operation result including protocol and local completion timeouts.
 */
SdoOperationResult readRemoteU32(lely::canopen::AsyncMaster& master,
                                 std::uint16_t index, std::uint8_t subindex,
                                 std::uint32_t& value)
{
    struct ReadState {
        std::mutex mutex;
        std::condition_variable condition;
        bool completed = false;
        std::error_code error;
        std::uint32_t value = 0;
    };

    const auto state = std::make_shared<ReadState>();
    std::error_code submit_error;
    master.SubmitRead<std::uint32_t>(
        master.GetExecutor(), CANOPEN_SLAVE_NODE_ID, index, subindex,
        [state](std::uint8_t, std::uint16_t, std::uint8_t,
                std::error_code error, std::uint32_t read_value) noexcept {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->completed = true;
                state->error = error;
                state->value = read_value;
            }
            state->condition.notify_all();
        },
        std::chrono::milliseconds(kSdoTimeoutMs), submit_error);
    if (submit_error) {
        spdlog::error("A03 unable to submit SDO read 0x{:04x}:{:02x}: {}",
                      index, static_cast<unsigned int>(subindex),
                      submit_error.message());
        return SdoOperationResult::FAILED;
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(
            lock,
            std::chrono::milliseconds(kSdoTimeoutMs
                                      + kSdoCompletionMarginMs),
            [state]() { return state->completed; })) {
        spdlog::error(
            "A03 SDO read completion 0x{:04x}:{:02x} timed out; remote "
            "transaction state is unknown",
            index, static_cast<unsigned int>(subindex));
        return SdoOperationResult::WAIT_TIMEOUT;
    }
    if (state->error) {
        if (lely::canopen::sdo_errc(state->error)
            == lely::canopen::SdoErrc::TIMEOUT) {
            spdlog::error("A03 SDO read 0x{:04x}:{:02x} timed out: {}", index,
                          static_cast<unsigned int>(subindex),
                          state->error.message());
            return SdoOperationResult::SDO_TIMEOUT;
        }
        spdlog::error("A03 SDO read 0x{:04x}:{:02x} failed: {}", index,
                      static_cast<unsigned int>(subindex),
                      state->error.message());
        return SdoOperationResult::FAILED;
    }

    value = state->value;
    return SdoOperationResult::SUCCESS;
}

/**
 * @brief Write an UNSIGNED32 object on the remote node through SDO.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param index Remote object index.
 * @param subindex Remote object sub-index.
 * @param value Value to download.
 * @return Operation result including protocol and local completion timeouts.
 */
SdoOperationResult writeRemoteU32(lely::canopen::AsyncMaster& master,
                                  std::uint16_t index,
                                  std::uint8_t subindex,
                                  std::uint32_t value)
{
    struct WriteState {
        std::mutex mutex;
        std::condition_variable condition;
        bool completed = false;
        std::error_code error;
    };

    const auto state = std::make_shared<WriteState>();
    std::error_code submit_error;
    master.SubmitWrite(
        master.GetExecutor(), CANOPEN_SLAVE_NODE_ID, index, subindex, value,
        [state](std::uint8_t, std::uint16_t, std::uint8_t,
                std::error_code error) noexcept {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->completed = true;
                state->error = error;
            }
            state->condition.notify_all();
        },
        std::chrono::milliseconds(kSdoTimeoutMs), submit_error);
    if (submit_error) {
        spdlog::error("A03 unable to submit SDO write 0x{:04x}:{:02x}: {}",
                      index, static_cast<unsigned int>(subindex),
                      submit_error.message());
        return SdoOperationResult::FAILED;
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(
            lock,
            std::chrono::milliseconds(kSdoTimeoutMs
                                      + kSdoCompletionMarginMs),
            [state]() { return state->completed; })) {
        spdlog::error(
            "A03 SDO write completion 0x{:04x}:{:02x} timed out; remote "
            "state is unknown",
            index, static_cast<unsigned int>(subindex));
        return SdoOperationResult::WAIT_TIMEOUT;
    }
    if (state->error) {
        if (lely::canopen::sdo_errc(state->error)
            == lely::canopen::SdoErrc::TIMEOUT) {
            spdlog::error("A03 SDO write 0x{:04x}:{:02x} timed out: {}", index,
                          static_cast<unsigned int>(subindex),
                          state->error.message());
            return SdoOperationResult::SDO_TIMEOUT;
        }
        spdlog::error("A03 SDO write 0x{:04x}:{:02x} failed: {}", index,
                      static_cast<unsigned int>(subindex),
                      state->error.message());
        return SdoOperationResult::FAILED;
    }

    return SdoOperationResult::SUCCESS;
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
    std::error_code error;
    value = master.RpdoMapped(CANOPEN_SLAVE_NODE_ID)[index][subindex]
                .Read<std::uint32_t>(error);
    if (error) {
        spdlog::error(
            "A03 unable to read TPDO-mapped object 0x{:04x}:{:02x}: {}",
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
    for (unsigned int attempt = 0; attempt < kConsistencyRetryCount;
         ++attempt) {
        TpdoSample sample;
        std::size_t generation = 0;
        if (!getLatestTpdoSample(state, sample, generation)) {
            spdlog::error("A03 TPDO1 has no valid sample for OD comparison");
            return false;
        }

        const std::uint32_t raw_value1 = decodeLe32(sample.payload.data());
        const std::uint32_t raw_value2 =
            decodeLe32(sample.payload.data() + 4U);

        std::uint32_t mapped_value1 = 0;
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

        std::uint32_t sdo_value1 = 0;
        const SdoOperationResult read1 =
            readRemoteU32(master, kTpdoValue1Index, kTpdoValue1Subindex,
                          sdo_value1);
        if (read1 == SdoOperationResult::WAIT_TIMEOUT) {
            completion_wait_timed_out = true;
            return false;
        }
        if (read1 != SdoOperationResult::SUCCESS) {
            return false;
        }

        std::uint32_t sdo_value2 = 0;
        const SdoOperationResult read2 =
            readRemoteU32(master, kTpdoValue2Index, kTpdoValue2Subindex,
                          sdo_value2);
        if (read2 == SdoOperationResult::WAIT_TIMEOUT) {
            completion_wait_timed_out = true;
            return false;
        }
        if (read2 != SdoOperationResult::SUCCESS) {
            return false;
        }

        if (!isTpdoGenerationStable(state, generation)) {
            spdlog::info(
                "A03 TPDO1 changed during OD comparison; retrying ({}/{})",
                attempt + 1U, kConsistencyRetryCount);
            continue;
        }

        if (mapped_value1 != raw_value1 || mapped_value2 != raw_value2) {
            spdlog::error(
                "A03 TPDO1 mapped value mismatch: raw=(0x{:08x},0x{:08x}) "
                "mapped=(0x{:08x},0x{:08x})",
                raw_value1, raw_value2, mapped_value1, mapped_value2);
            return false;
        }
        if (sdo_value1 != raw_value1 || sdo_value2 != raw_value2) {
            spdlog::error(
                "A03 TPDO1/OD mismatch: TPDO=(0x{:08x},0x{:08x}) "
                "SDO=(0x{:08x},0x{:08x})",
                raw_value1, raw_value2, sdo_value1, sdo_value2);
            return false;
        }

        spdlog::info(
            "A03 TPDO1 mapping/OD verified: 0x2100:00=0x{:08x} "
            "0x2101:00=0x{:08x}",
            raw_value1, raw_value2);
        return true;
    }

    spdlog::error(
        "A03 TPDO1 changed during all {} OD comparison attempts",
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
        std::lock_guard<std::mutex> lock(state->mutex);
        state->completed = false;
        state->pdo_number = 0;
        state->error.clear();
        state->payload.fill(0);
        state->length = 0;
    }

    std::error_code error;
    auto mapped = master.TpdoMapped(CANOPEN_SLAVE_NODE_ID)[kRpdoTestIndex]
                                    [kRpdoTestSubindex];
    mapped.Write(value, error);
    if (error) {
        spdlog::error("A03 unable to write RPDO-mapped 0x2200:00: {}",
                      error.message());
        return false;
    }
    mapped.WriteEvent(error);
    if (error) {
        spdlog::error("A03 unable to trigger RPDO1 transmission: {}",
                      error.message());
        return false;
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(
            lock, std::chrono::milliseconds(kRpdoTransmitTimeoutMs),
            [state]() { return state->completed; })) {
        spdlog::error("A03 RPDO1 transmission callback timed out");
        return false;
    }
    if (state->error) {
        spdlog::error("A03 RPDO1 transmission failed: {}",
                      state->error.message());
        return false;
    }
    if (state->pdo_number != kPdoNumber
        || state->length != kRpdoPayloadLength) {
        spdlog::error(
            "A03 RPDO1 transmission mismatch: pdo={} expected_dlc={} "
            "actual_dlc={}",
            state->pdo_number, kRpdoPayloadLength, state->length);
        return false;
    }

    const std::uint32_t payload_value = decodeLe32(state->payload.data());
    if (payload_value != value) {
        spdlog::error(
            "A03 RPDO1 payload mismatch: expected=0x{:08x} actual=0x{:08x}",
            value, payload_value);
        return false;
    }

    spdlog::info("A03 RPDO1 transmitted: 0x2200:00=0x{:08x}", value);
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
    const SdoOperationResult write_result =
        writeRemoteU32(master, kRpdoTestIndex, kRpdoTestSubindex,
                       original_value);
    if (write_result == SdoOperationResult::WAIT_TIMEOUT) {
        completion_wait_timed_out = true;
        return false;
    }
    if (write_result == SdoOperationResult::FAILED) {
        return false;
    }
    if (write_result == SdoOperationResult::SDO_TIMEOUT) {
        spdlog::warn(
            "A03 restore SDO response timed out; verifying 0x2200:00 by "
            "read-back");
    }

    std::uint32_t restored_value = 0;
    const SdoOperationResult read_result =
        readRemoteU32(master, kRpdoTestIndex, kRpdoTestSubindex,
                      restored_value);
    if (read_result == SdoOperationResult::WAIT_TIMEOUT) {
        completion_wait_timed_out = true;
        return false;
    }
    if (read_result != SdoOperationResult::SUCCESS) {
        return false;
    }
    if (restored_value != original_value) {
        spdlog::error(
            "A03 restored 0x2200:00 mismatch: expected=0x{:08x} "
            "actual=0x{:08x}",
            original_value, restored_value);
        return false;
    }

    spdlog::info("A03 original 0x2200:00 restored: 0x{:08x}",
                 restored_value);
    return true;
}

} // namespace

int pdoProcess(lely::canopen::AsyncMaster& master)
{
    const auto receive_state = std::make_shared<TpdoReceiveState>();
    const auto transmit_state = std::make_shared<RpdoTransmitState>();
    registerPdoCallbacks(master, receive_state, transmit_state);

    int result = 0;
    bool original_value_saved = false;
    bool completion_wait_timed_out = false;
    std::uint32_t original_value = 0;

    if (!issueNmtCommand(master, lely::canopen::NmtCommand::START,
                         CANOPEN_MASTER_NODE_ID,
                         "A03 local master NMT Start")) {
        result = 1;
    }

    if (result == 0
        && !issueNmtCommand(master, lely::canopen::NmtCommand::START,
                            CANOPEN_SLAVE_NODE_ID,
                            "A03 slave NMT Start")) {
        result = 1;
    }
    /*
    * TPDO1 is event-driven (transmission type 254) with a 1000 ms event
    * timer. After the slave enters Operational, CANopenNode transmits TPDO1
    * periodically without a request from the master.
    */
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

    if (result == 0) {
        const SdoOperationResult read_result =
            readRemoteU32(master, kRpdoTestIndex, kRpdoTestSubindex,
                          original_value);
        if (read_result == SdoOperationResult::WAIT_TIMEOUT) {
            completion_wait_timed_out = true;
            result = 1;
        } else if (read_result != SdoOperationResult::SUCCESS) {
            result = 1;
        } else {
            original_value_saved = true;
            spdlog::info("A03 saved original 0x2200:00=0x{:08x}",
                         original_value);
        }
    }

    const std::uint32_t probe_value =
        original_value == kRpdoProbeValue ? kRpdoAlternateValue
                                           : kRpdoProbeValue;
    if (result == 0) {
        if (!sendRpdoValue(master, transmit_state, probe_value)) {
            result = 1;
        }
    }

    if (result == 0) {
        std::uint32_t read_back_value = 0;
        const SdoOperationResult read_result =
            readRemoteU32(master, kRpdoTestIndex, kRpdoTestSubindex,
                          read_back_value);
        if (read_result == SdoOperationResult::WAIT_TIMEOUT) {
            completion_wait_timed_out = true;
            result = 1;
        } else if (read_result != SdoOperationResult::SUCCESS) {
            result = 1;
        } else if (read_back_value != probe_value) {
            spdlog::error(
                "A03 RPDO1/OD mismatch: expected=0x{:08x} actual=0x{:08x}",
                probe_value, read_back_value);
            result = 1;
        } else {
            spdlog::info("A03 RPDO1 OD update verified: 0x2200:00=0x{:08x}",
                         read_back_value);
        }
    }

    if (original_value_saved) {
        if (!completion_wait_timed_out
            && !restoreRpdoTestValue(master, original_value,
                                     completion_wait_timed_out)) {
            spdlog::error("A03 0x2200:00 restoration was not verified");
            result = 1;
        }

        if (completion_wait_timed_out) {
            spdlog::warn(
                "A03 local SDO completion state is unknown; attempting "
                "best-effort RPDO restoration without SDO verification");
            if (!sendRpdoValue(master, transmit_state, original_value)) {
                spdlog::error("A03 best-effort RPDO restoration failed");
            }
            result = 1;
        }
    }

    clearPdoCallbacks(master);

    if (result == 0) {
        spdlog::info("A03 RPDO/TPDO test passed");
    }
    return result;
}
