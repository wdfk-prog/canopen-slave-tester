/**
 * @file
 * @brief Implements user object dictionary SDO read/write verification.
 */

#include "sdo_process.h"

#include "canopen_config.h"
#include "canopen_sdo.h"

#include <lely/coapp/master.hpp>
#include <spdlog/spdlog.h>

#include <cstdint>

namespace {

/** User OD object selected as the reversible SDO read/write test target. */
constexpr std::uint16_t kTestObjectIndex = 0x2200;
/** The demo control object is scalar and therefore uses sub-index zero. */
constexpr std::uint8_t kTestObjectSubindex = 0x00;
/** Primary probe pattern chosen to make byte-order mistakes visually obvious. */
constexpr std::uint32_t kProbeValue = 0x12345678;
/** Alternate probe avoids accidentally writing the value that was already
 * present before the test. */
constexpr std::uint32_t kAlternateProbeValue = 0x87654321;
/**
 * @brief Read the slave user control value through the common remote SDO helper.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param value Receives the value read from 0x2200:00 on success.
 * @return Operation result including SDO and local completion timeouts.
 */
SdoOperationResult readControlValue(lely::canopen::AsyncMaster& master,
                                    std::uint32_t& value)
{
    return readRemoteSdo<std::uint32_t>(
        master, CANOPEN_SLAVE_NODE_ID, kTestObjectIndex,
        kTestObjectSubindex, value);
}

/**
 * @brief Write the slave user control value through the common remote SDO helper.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param value Value written to 0x2200:00.
 * @return Operation result including SDO and local completion timeouts.
 */
SdoOperationResult writeControlValue(lely::canopen::AsyncMaster& master,
                                     std::uint32_t value)
{
    return writeRemoteSdo<std::uint32_t>(
        master, CANOPEN_SLAVE_NODE_ID, kTestObjectIndex,
        kTestObjectSubindex, value);
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
    /* First restore the saved value; a protocol timeout still permits a
     * read-back because the callback completed and the SDO channel is known. */
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

    /* Zero is only a neutral placeholder; successful read-back overwrites it
     * before comparison with the saved original value. */
    std::uint32_t restored_value = 0;
    /* Keep the read result separate so timeout class controls cleanup policy. */
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
    /* Step 1: save the current object value before making any reversible test
     * modification. Zero is only initial storage until the SDO upload ends. */
    std::uint32_t original_value = 0;
    /* Preserve the exact failure class because WAIT_TIMEOUT has stricter
     * cleanup rules than a normal SDO abort/timeout. */
    const SdoOperationResult initial_read_result =
        readControlValue(master, original_value);
    if (initial_read_result != SdoOperationResult::SUCCESS) {
        return 1;
    }
    spdlog::info("SDO object-access validation user OD test started: original 0x2200:00=0x{:08x}",
                 original_value);

    /* Step 2: select a value guaranteed to differ from the saved value so the
     * test proves that a write actually changed the remote OD entry. */
    const std::uint32_t test_value =
        original_value == kProbeValue ? kAlternateProbeValue : kProbeValue;
    /* Submit the reversible test write and retain its precise completion state. */
    const SdoOperationResult test_write_result =
        writeControlValue(master, test_value);
    if (test_write_result == SdoOperationResult::WAIT_TIMEOUT) {
        spdlog::error(
            "SDO object-access validation test aborted: 0x2200:00 write state is unknown; "
            "verify the target value before rerunning the test");
        return 1;
    }
    if (test_write_result == SdoOperationResult::SDO_TIMEOUT) {
        spdlog::error(
            "SDO object-access validation test write timed out at the SDO protocol level; attempting "
            "to restore the original value");
        if (restoreControlValue(master, original_value)
            != SdoOperationResult::SUCCESS) {
            spdlog::error(
                "SDO object-access validation test failed and 0x2200:00 restoration was not "
                "verified");
        }
        return 1;
    }
    if (test_write_result != SdoOperationResult::SUCCESS) {
        return 1;
    }

    /* Step 3: read the temporary value back. result starts at success and is
     * retained through restoration so cleanup still runs after assertions. */
    int result = 0;
    /* false means the SDO channel completion state is still known and cleanup
     * transactions may be issued safely. */
    bool completion_wait_timed_out = false;
    /* Neutral storage overwritten by a successful SDO upload. */
    std::uint32_t read_back_value = 0;
    /* Store the exact read outcome for value and cleanup decisions. */
    const SdoOperationResult read_back_result =
        readControlValue(master, read_back_value);
    if (read_back_result == SdoOperationResult::WAIT_TIMEOUT) {
        completion_wait_timed_out = true;
        result = 1;
    } else if (read_back_result != SdoOperationResult::SUCCESS) {
        result = 1;
    } else if (read_back_value != test_value) {
        spdlog::error(
            "SDO object-access validation write/read-back mismatch: expected=0x{:08x} "
            "actual=0x{:08x}",
            test_value, read_back_value);
        result = 1;
    } else {
        spdlog::info("SDO object-access validation write/read-back verified: 0x{:08x}",
                     read_back_value);
    }

    if (completion_wait_timed_out) {
        spdlog::error(
            "SDO object-access validation test cannot safely restore 0x2200:00 after a local "
            "completion wait timeout; reset or inspect the target before "
            "continuing");
        return 1;
    }

    /* Step 4: restore and independently verify the original value before the
     * process can report PASS. */
    const SdoOperationResult restore_result =
        restoreControlValue(master, original_value);
    if (restore_result != SdoOperationResult::SUCCESS) {
        spdlog::error(
            "SDO object-access validation test failed and 0x2200:00 restoration was not verified");
        return 1;
    }

    if (result == 0) {
        spdlog::info("SDO object-access validation user OD test passed");
    }
    return result;
}
