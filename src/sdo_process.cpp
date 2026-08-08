/**
 * @file
 * @brief Implements user object dictionary SDO read/write verification.
 */

#include "sdo_process.h"

#include "canopen_config.h"

#include <lely/coapp/master.hpp>
#include <lely/coapp/sdo_error.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <system_error>

namespace {

constexpr std::uint16_t kControlValueIndex = 0x2200;
constexpr std::uint8_t kControlValueSubindex = 0x00;
constexpr std::uint32_t kPrimaryTestValue = 0x12345678;
constexpr std::uint32_t kAlternateTestValue = 0x87654321;
constexpr int kCompletionMarginMs = 500;

enum class SdoOperationResult {
    SUCCESS,
    FAILED,
    SDO_TIMEOUT,
    WAIT_TIMEOUT,
};

/**
 * @brief Read the slave user control value through SDO.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param value Receives the value read from 0x2200:00 on success.
 * @return Operation result including SDO and local completion timeouts.
 */
SdoOperationResult readControlValue(lely::canopen::AsyncMaster& master,
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
        master.GetExecutor(), CANOPEN_SLAVE_NODE_ID, kControlValueIndex,
        kControlValueSubindex,
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
        std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS), submit_error);
    if (submit_error) {
        spdlog::error("Unable to submit SDO read for 0x2200:00: {}",
                      submit_error.message());
        return SdoOperationResult::FAILED;
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(
            lock,
            std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS
                                      + kCompletionMarginMs),
            [state]() { return state->completed; })) {
        spdlog::error(
            "SDO read completion for 0x2200:00 timed out; remote state is "
            "unknown");
        return SdoOperationResult::WAIT_TIMEOUT;
    }
    if (state->error) {
        if (lely::canopen::sdo_errc(state->error)
            == lely::canopen::SdoErrc::TIMEOUT) {
            spdlog::error("SDO read for 0x2200:00 timed out: {}",
                          state->error.message());
            return SdoOperationResult::SDO_TIMEOUT;
        }
        spdlog::error("SDO read for 0x2200:00 failed: {}",
                      state->error.message());
        return SdoOperationResult::FAILED;
    }

    value = state->value;
    return SdoOperationResult::SUCCESS;
}

/**
 * @brief Write the slave user control value through SDO.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param value Value written to 0x2200:00.
 * @return Operation result including SDO and local completion timeouts.
 */
SdoOperationResult writeControlValue(lely::canopen::AsyncMaster& master,
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
        master.GetExecutor(), CANOPEN_SLAVE_NODE_ID, kControlValueIndex,
        kControlValueSubindex, value,
        [state](std::uint8_t, std::uint16_t, std::uint8_t,
                std::error_code error) noexcept {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->completed = true;
                state->error = error;
            }
            state->condition.notify_all();
        },
        std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS), submit_error);
    if (submit_error) {
        spdlog::error("Unable to submit SDO write for 0x2200:00: {}",
                      submit_error.message());
        return SdoOperationResult::FAILED;
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(
            lock,
            std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS
                                      + kCompletionMarginMs),
            [state]() { return state->completed; })) {
        spdlog::error(
            "SDO write completion for 0x2200:00 timed out; remote state is "
            "unknown");
        return SdoOperationResult::WAIT_TIMEOUT;
    }
    if (state->error) {
        if (lely::canopen::sdo_errc(state->error)
            == lely::canopen::SdoErrc::TIMEOUT) {
            spdlog::error("SDO write for 0x2200:00 timed out: {}",
                          state->error.message());
            return SdoOperationResult::SDO_TIMEOUT;
        }
        spdlog::error("SDO write for 0x2200:00 failed: {}",
                      state->error.message());
        return SdoOperationResult::FAILED;
    }

    return SdoOperationResult::SUCCESS;
}

/**
 * @brief Restore and verify a previously saved control value.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param original_value Value to restore to 0x2200:00.
 * @return Operation result including SDO and local completion timeouts.
 */
SdoOperationResult restoreControlValue(lely::canopen::AsyncMaster& master,
                                       std::uint32_t original_value)
{
    const SdoOperationResult write_result =
        writeControlValue(master, original_value);
    if (write_result == SdoOperationResult::WAIT_TIMEOUT
        || write_result == SdoOperationResult::FAILED) {
        spdlog::error("Unable to restore original 0x2200:00 value");
        return write_result;
    }
    if (write_result == SdoOperationResult::SDO_TIMEOUT) {
        spdlog::warn(
            "Restore SDO response timed out; verifying 0x2200:00 by read-back");
    }

    std::uint32_t restored_value = 0;
    const SdoOperationResult read_result =
        readControlValue(master, restored_value);
    if (read_result != SdoOperationResult::SUCCESS) {
        spdlog::error("Unable to verify restored 0x2200:00 value");
        return read_result;
    }
    if (restored_value != original_value) {
        spdlog::error(
            "Restored 0x2200:00 value mismatch: expected=0x{:08x} "
            "actual=0x{:08x}",
            original_value, restored_value);
        return SdoOperationResult::FAILED;
    }

    spdlog::info("Original 0x2200:00 value restored: 0x{:08x}",
                 restored_value);
    return SdoOperationResult::SUCCESS;
}

} // namespace

int sdoProcess(lely::canopen::AsyncMaster& master)
{
    std::uint32_t original_value = 0;
    const SdoOperationResult initial_read_result =
        readControlValue(master, original_value);
    if (initial_read_result != SdoOperationResult::SUCCESS) {
        return 1;
    }
    spdlog::info("A02 SDO user OD test started: original 0x2200:00=0x{:08x}",
                 original_value);

    const std::uint32_t test_value =
        original_value == kPrimaryTestValue ? kAlternateTestValue
                                            : kPrimaryTestValue;
    const SdoOperationResult test_write_result =
        writeControlValue(master, test_value);
    if (test_write_result == SdoOperationResult::WAIT_TIMEOUT) {
        spdlog::error(
            "A02 SDO test aborted: 0x2200:00 write state is unknown; "
            "verify the target value before rerunning the test");
        return 1;
    }
    if (test_write_result == SdoOperationResult::SDO_TIMEOUT) {
        spdlog::error(
            "A02 test write timed out at the SDO protocol level; attempting "
            "to restore the original value");
        if (restoreControlValue(master, original_value)
            != SdoOperationResult::SUCCESS) {
            spdlog::error(
                "A02 SDO test failed and 0x2200:00 restoration was not "
                "verified");
        }
        return 1;
    }
    if (test_write_result != SdoOperationResult::SUCCESS) {
        return 1;
    }

    int result = 0;
    bool completion_wait_timed_out = false;
    std::uint32_t read_back_value = 0;
    const SdoOperationResult read_back_result =
        readControlValue(master, read_back_value);
    if (read_back_result == SdoOperationResult::WAIT_TIMEOUT) {
        completion_wait_timed_out = true;
        result = 1;
    } else if (read_back_result != SdoOperationResult::SUCCESS) {
        result = 1;
    } else if (read_back_value != test_value) {
        spdlog::error(
            "A02 SDO write/read-back mismatch: expected=0x{:08x} "
            "actual=0x{:08x}",
            test_value, read_back_value);
        result = 1;
    } else {
        spdlog::info("A02 SDO write/read-back verified: 0x{:08x}",
                     read_back_value);
    }

    if (completion_wait_timed_out) {
        spdlog::error(
            "A02 SDO test cannot safely restore 0x2200:00 after a local "
            "completion wait timeout; reset or inspect the target before "
            "continuing");
        return 1;
    }

    const SdoOperationResult restore_result =
        restoreControlValue(master, original_value);
    if (restore_result != SdoOperationResult::SUCCESS) {
        spdlog::error(
            "A02 SDO test failed and 0x2200:00 restoration was not verified");
        return 1;
    }

    if (result == 0) {
        spdlog::info("A02 SDO user OD test passed");
    }
    return result;
}
