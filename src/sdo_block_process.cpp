/**
 * @file
 * @brief Implements SDO server block-transfer validation.
 */

#include "sdo_block_process.h"

#include "canopen_config.h"
#include "canopen_master.h"
#include "canopen_nmt.h"
#include "canopen_sdo.h"

#include <lely/coapp/master.hpp>
#include <lely/coapp/sdo.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <system_error>
#include <vector>

namespace {

/** Test-only variable-length DOMAIN exposed by the MCU demo OD. */
constexpr std::uint16_t kBlockObjectIndex = 0x2304U;
/** DOMAIN fixture uses sub-index zero because it is a VAR object. */
constexpr std::uint8_t kBlockObjectSubindex = 0x00U;
/** Read-only standard object used for the illegal-access abort case. */
constexpr std::uint16_t kReadOnlyIndex = 0x1000U;
/** Device Type is a VAR at sub-index zero. */
constexpr std::uint8_t kReadOnlySubindex = 0x00U;
/** Largest payload supported by the MCU test fixture. */
constexpr std::size_t kMaxPayloadSize = 2048U;
/** Number of 2048-byte round trips in the functional stability case. */
constexpr unsigned int kStressIterations = 100U;
/** Short protocol timeout used only while the MCU is deliberately stopped. */
constexpr std::uint32_t kTimeoutProbeMs = 500U;
/** Extra local completion margin for the deliberate timeout probe. */
constexpr std::uint32_t kTimeoutProbeMarginMs = 500U;
/** FNV-1a 32-bit offset basis shared with the MCU SDO client validation payload convention. */
constexpr std::uint32_t kFnvOffset = 2166136261UL;
/** FNV-1a 32-bit prime shared with the MCU SDO client validation payload convention. */
constexpr std::uint32_t kFnvPrime = 16777619UL;
/** Base deterministic payload seed for normal block-transfer cases. */
constexpr std::uint32_t kPatternSeed = 0xB02A5A5AU;

/** One callback snapshot for the explicit client-abort case. */
struct AbortState {
    std::mutex mutex; /**< Protects completion/error publication. */
    std::condition_variable condition; /**< Wakes the process after Lely completion. */
    bool completed = false; /**< false until the self-owned Lely request completes. */
    std::error_code error; /**< SDO abort code delivered by the completion callback. */
};

/** Return one deterministic payload byte for a seed and absolute offset. */
std::uint8_t patternByte(std::uint32_t seed, std::size_t offset) noexcept
{
    const std::uint8_t seed_byte = static_cast<std::uint8_t>(
        seed >> ((offset & 0x03U) * 8U));
    return static_cast<std::uint8_t>(
        seed_byte ^ static_cast<std::uint8_t>(offset));
}

/** Build a deterministic payload without relying on random state. */
std::vector<std::uint8_t> makePayload(std::size_t size, std::uint32_t seed)
{
    std::vector<std::uint8_t> payload(size);
    for (std::size_t i = 0U; i < size; ++i) {
        payload[i] = patternByte(seed, i);
    }
    return payload;
}

/** Calculate the application-level FNV-1a checksum used for evidence. */
std::uint32_t checksum(const std::vector<std::uint8_t>& data) noexcept
{
    std::uint32_t value = kFnvOffset;
    for (const std::uint8_t byte : data) {
        value ^= static_cast<std::uint32_t>(byte);
        value *= kFnvPrime;
    }
    return value;
}

/** Mark the SDO channel unsafe if the local waiter lost completion evidence. */
void updateChannelState(SdoOperationResult result, bool& channel_known) noexcept
{
    if (result == SdoOperationResult::WAIT_TIMEOUT) {
        channel_known = false;
    }
}

/** Verify size, independent checksum and byte-for-byte payload equality. */
bool validatePayload(const char* case_name,
                     const std::vector<std::uint8_t>& expected,
                     const std::vector<std::uint8_t>& actual)
{
    if (actual.size() != expected.size()) {
        spdlog::error("{} size mismatch: expected={} actual={}", case_name,
                      expected.size(), actual.size());
        return false;
    }
    const std::uint32_t expected_checksum = checksum(expected);
    const std::uint32_t actual_checksum = checksum(actual);
    if (actual_checksum != expected_checksum) {
        spdlog::error(
            "{} checksum mismatch: expected=0x{:08x} actual=0x{:08x}",
            case_name, expected_checksum, actual_checksum);
        return false;
    }
    if (actual != expected) {
        spdlog::error("{} byte-for-byte comparison failed", case_name);
        return false;
    }
    return true;
}

/** Execute one explicit block download followed by an explicit block upload. */
bool runBlockRoundTrip(lely::canopen::AsyncMaster& master,
                       const char* case_name, std::size_t size,
                       std::uint32_t seed, bool& channel_known)
{
    const std::vector<std::uint8_t> expected = makePayload(size, seed);
    std::vector<std::uint8_t> actual;

    SdoOperationResult result = writeRemoteBlockSdo(
        master, CANOPEN_SLAVE_NODE_ID, kBlockObjectIndex,
        kBlockObjectSubindex, expected);
    updateChannelState(result, channel_known);
    if (result != SdoOperationResult::SUCCESS) {
        spdlog::error("{} block download failed", case_name);
        return false;
    }

    result = readRemoteBlockSdo(
        master, CANOPEN_SLAVE_NODE_ID, kBlockObjectIndex,
        kBlockObjectSubindex, actual);
    updateChannelState(result, channel_known);
    if (result != SdoOperationResult::SUCCESS
        || !validatePayload(case_name, expected, actual)) {
        spdlog::error("{} block upload/compare failed", case_name);
        return false;
    }

    spdlog::info("{} passed: size={} checksum=0x{:08x}", case_name,
                 expected.size(), checksum(expected));
    return true;
}

/** Verify a read-only object rejects an explicit block download. */
bool runIllegalAccessCase(lely::canopen::AsyncMaster& master,
                          bool& channel_known)
{
    const std::vector<std::uint8_t> payload = makePayload(32U, kPatternSeed);
    std::error_code error;
    const SdoOperationResult result = writeRemoteBlockSdo(
        master, CANOPEN_SLAVE_NODE_ID, kReadOnlyIndex, kReadOnlySubindex,
        payload, std::chrono::milliseconds(CANOPEN_SDO_TIMEOUT_MS),
        std::chrono::milliseconds(CANOPEN_SDO_COMPLETION_MARGIN_MS), &error);

    updateChannelState(result, channel_known);
    if (result != SdoOperationResult::FAILED
        || lely::canopen::sdo_errc(error) != lely::canopen::SdoErrc::NO_WRITE) {
        spdlog::error(
            "SDO block illegal-access recovery expected NO_WRITE (0x06010002), result={} error={}",
            static_cast<int>(result), error.message());
        return false;
    }

    spdlog::info("SDO block illegal-access recovery illegal-access abort passed: {}", error.message());
    return runBlockRoundTrip(master, "SDO block illegal-access recovery", 32U,
                             kPatternSeed ^ 0x06010002U, channel_known);
}

/** Restore the MCU from Stopped to Pre-operational for SDO recovery. */
bool restorePreOperational(lely::canopen::AsyncMaster& master)
{
    return issueNmtCommandAndWaitForState(
        master, lely::canopen::NmtCommand::ENTER_PREOP,
        CANOPEN_SLAVE_NODE_ID, lely::canopen::NmtState::PREOP,
        std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS),
        "SDO server block-transfer validation restore Pre-operational");
}

/** Deliberately stop the server, require an SDO timeout, then restore it. */
bool runTimeoutRecoveryCase(lely::canopen::AsyncMaster& master,
                            bool& channel_known, bool& node_stopped)
{
    if (!issueNmtCommandAndWaitForState(
            master, lely::canopen::NmtCommand::STOP,
            CANOPEN_SLAVE_NODE_ID, lely::canopen::NmtState::STOP,
            std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS),
            "SDO block stopped-node timeout recovery stop slave")) {
        return false;
    }
    node_stopped = true;

    std::vector<std::uint8_t> ignored;
    std::error_code error;
    const SdoOperationResult result = readRemoteBlockSdo(
        master, CANOPEN_SLAVE_NODE_ID, kBlockObjectIndex,
        kBlockObjectSubindex, ignored,
        std::chrono::milliseconds(kTimeoutProbeMs),
        std::chrono::milliseconds(kTimeoutProbeMarginMs), &error);
    updateChannelState(result, channel_known);

    const bool restored = restorePreOperational(master);
    if (restored) {
        node_stopped = false;
    }
    if (!restored) {
        spdlog::error("SDO block stopped-node timeout recovery could not restore the slave to Pre-operational");
        return false;
    }
    if (result != SdoOperationResult::SDO_TIMEOUT
        || lely::canopen::sdo_errc(error) != lely::canopen::SdoErrc::TIMEOUT) {
        spdlog::error(
            "SDO block stopped-node timeout recovery expected protocol SDO timeout, result={} error={}",
            static_cast<int>(result), error.message());
        return false;
    }

    spdlog::info("SDO block stopped-node timeout recovery transfer timeout observed and NMT state restored");
    return runBlockRoundTrip(master, "SDO block stopped-node timeout recovery", 32U,
                             kPatternSeed ^ 0x05040000U, channel_known);
}

/**
 * Establish a deterministic remote SDO-server readiness barrier after abort.
 *
 * CANopenNode disables SDO service in Stopped. A confirmed Stop followed by a
 * confirmed Pre-operational transition guarantees at least one remote process
 * cycle in which pending SDO receive state is discarded, without relying on a
 * fixed sleep or performing a communication reset.
 */
bool synchronizeSdoServerAfterAbort(CanopenTestMaster& master,
                                    bool& channel_known,
                                    bool& node_stopped)
{
    if (!issueNmtCommandAndWaitForState(
            master, lely::canopen::NmtCommand::STOP,
            CANOPEN_SLAVE_NODE_ID, lely::canopen::NmtState::STOP,
            std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS),
            "SDO block client-abort recovery: stop slave for recovery barrier")) {
        /* The command may have reached the node even though the fresh state
         * indication was not observed, so remote SDO readiness is unknown. */
        channel_known = false;
        spdlog::error(
            "SDO block client-abort recovery could not establish the remote SDO recovery barrier");
        return false;
    }
    node_stopped = true;

    if (!restorePreOperational(master)) {
        spdlog::error(
            "SDO block client-abort recovery could not restore Pre-operational after the recovery barrier");
        return false;
    }
    node_stopped = false;

    spdlog::info(
        "SDO block client-abort recovery barrier completed via Stop -> Pre-operational");
    return true;
}

/** Cancel one active Lely-owned block request with a client GENERAL abort. */
bool runClientAbortCase(CanopenTestMaster& master, bool& channel_known,
                        bool& node_stopped)
{
    const auto state = std::make_shared<AbortState>();
    const std::vector<std::uint8_t> payload =
        makePayload(kMaxPayloadSize, kPatternSeed ^ 0x08000000U);
    std::error_code submit_error;

    master.SubmitBlockWrite(
        master.GetExecutor(), CANOPEN_SLAVE_NODE_ID, kBlockObjectIndex,
        kBlockObjectSubindex, payload,
        [state](std::uint8_t, std::uint16_t, std::uint8_t,
                std::error_code error) noexcept {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->completed = true;
                state->error = error;
            }
            state->condition.notify_all();
        },
        std::chrono::milliseconds(CANOPEN_SDO_TIMEOUT_MS), submit_error);
    if (submit_error) {
        spdlog::error("SDO block client-abort recovery block request submission failed: {}",
                      submit_error.message());
        return false;
    }

    /* No other SDO server block-transfer operation is active here, so cancelling the per-node queue
     * targets the request just submitted while preserving Lely ownership. */
    if (!master.cancelRemoteSdoRequests(
            CANOPEN_SLAVE_NODE_ID, lely::canopen::SdoErrc::ERROR)) {
        channel_known = false;
        spdlog::error("SDO block client-abort recovery remote Client-SDO service is unavailable");
        return false;
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(
            lock,
            std::chrono::milliseconds(CANOPEN_SDO_TIMEOUT_MS
                                      + CANOPEN_SDO_COMPLETION_MARGIN_MS),
            [state]() { return state->completed; })) {
        channel_known = false;
        spdlog::error(
            "SDO block client-abort recovery client-abort completion timed out; SDO channel state is unknown");
        return false;
    }

    if (lely::canopen::sdo_errc(state->error)
        != lely::canopen::SdoErrc::ERROR) {
        spdlog::error("SDO block client-abort recovery expected client GENERAL abort: error={}",
                      state->error.message());
        return false;
    }

    spdlog::info("SDO block client-abort recovery client abort observed: {}", state->error.message());
    lock.unlock();

    if (!synchronizeSdoServerAfterAbort(master, channel_known, node_stopped)) {
        return false;
    }

    return runBlockRoundTrip(master, "SDO block client-abort recovery",
                             kMaxPayloadSize,
                             kPatternSeed ^ 0x08000001U, channel_known);
}

/** Verify an expedited SDO works immediately after a block transfer. */
bool runExpeditedRegression(lely::canopen::AsyncMaster& master,
                            bool& channel_known)
{
    if (!runBlockRoundTrip(master, "SDO block-to-expedited regression block prerequisite", 32U,
                           kPatternSeed ^ 0x09U, channel_known)) {
        return false;
    }

    std::uint32_t device_type = 0U;
    const SdoOperationResult result = readRemoteSdo<std::uint32_t>(
        master, CANOPEN_SLAVE_NODE_ID, kReadOnlyIndex, kReadOnlySubindex,
        device_type);
    updateChannelState(result, channel_known);
    if (result != SdoOperationResult::SUCCESS) {
        spdlog::error("SDO block-to-expedited regression failed");
        return false;
    }

    spdlog::info("SDO block-to-expedited regression passed: device_type=0x{:08x}",
                 device_type);
    return true;
}

/** Verify a normal segmented upload works immediately after block transfer. */
bool runSegmentedRegression(lely::canopen::AsyncMaster& master,
                            bool& channel_known)
{
    const std::vector<std::uint8_t> expected =
        makePayload(kMaxPayloadSize, kPatternSeed ^ 0x10U);
    SdoOperationResult result = writeRemoteBlockSdo(
        master, CANOPEN_SLAVE_NODE_ID, kBlockObjectIndex,
        kBlockObjectSubindex, expected);
    updateChannelState(result, channel_known);
    if (result != SdoOperationResult::SUCCESS) {
        spdlog::error("SDO block-to-segmented regression block prerequisite download failed");
        return false;
    }

    std::vector<std::uint8_t> actual;
    result = readRemoteSdo<std::vector<std::uint8_t>>(
        master, CANOPEN_SLAVE_NODE_ID, kBlockObjectIndex,
        kBlockObjectSubindex, actual);
    updateChannelState(result, channel_known);
    if (result != SdoOperationResult::SUCCESS
        || !validatePayload("SDO block-to-segmented regression segmented upload", expected, actual)) {
        return false;
    }

    spdlog::info("SDO block-to-segmented regression passed");
    return true;
}

/** Repeat maximum-size block transfers with varying payload seeds. */
bool runStressCase(lely::canopen::AsyncMaster& master, bool& channel_known)
{
    for (unsigned int iteration = 0U; iteration < kStressIterations;
         ++iteration) {
        const std::uint32_t seed =
            kPatternSeed ^ (0x11000000U + iteration * 0x01010101U);
        if (!runBlockRoundTrip(master, "SDO block maximum-size stability loop stress", kMaxPayloadSize,
                               seed, channel_known)) {
            spdlog::error("SDO block maximum-size stability loop failed at iteration {}", iteration + 1U);
            return false;
        }
    }

    spdlog::info(
        "SDO block maximum-size stability loop functional stability passed: {} maximum-size round trips; "
        "runtime heap leak evidence requires target-side memory statistics",
        kStressIterations);
    return true;
}

/** Restore the original fixture payload and independently read it back. */
bool restoreOriginalPayload(lely::canopen::AsyncMaster& master,
                            const std::vector<std::uint8_t>& original,
                            bool& channel_known)
{
    SdoOperationResult result = writeRemoteBlockSdo(
        master, CANOPEN_SLAVE_NODE_ID, kBlockObjectIndex,
        kBlockObjectSubindex, original);
    updateChannelState(result, channel_known);
    if (result != SdoOperationResult::SUCCESS) {
        spdlog::error("SDO server block-transfer validation cleanup block download failed");
        return false;
    }

    std::vector<std::uint8_t> readback;
    result = readRemoteBlockSdo(
        master, CANOPEN_SLAVE_NODE_ID, kBlockObjectIndex,
        kBlockObjectSubindex, readback);
    updateChannelState(result, channel_known);
    if (result != SdoOperationResult::SUCCESS
        || !validatePayload("SDO server block-transfer validation cleanup", original, readback)) {
        spdlog::error("SDO server block-transfer validation cleanup read-back verification failed");
        return false;
    }

    spdlog::info("SDO server block-transfer validation cleanup restored the original 0x2304 payload");
    return true;
}

} // namespace

int sdoBlockProcess(CanopenTestMaster& master)
{
    bool channel_known = true;
    bool node_stopped = false;
    bool cases_passed = true;
    std::vector<std::uint8_t> original;

    SdoOperationResult result = readRemoteBlockSdo(
        master, CANOPEN_SLAVE_NODE_ID, kBlockObjectIndex,
        kBlockObjectSubindex, original);
    updateChannelState(result, channel_known);
    if (result != SdoOperationResult::SUCCESS || original.empty()
        || original.size() > kMaxPayloadSize) {
        spdlog::error(
            "SDO server block-transfer validation cannot save the original 0x2304 payload; verify MCU block fixture configuration");
        return 1;
    }

    spdlog::info("SDO server block-transfer validation saved original 0x2304 payload: size={} checksum=0x{:08x}",
                 original.size(), checksum(original));

    do {
        const struct {
            const char* name;
            std::size_t size;
            std::uint32_t seed;
        } functional_cases[] = {
            {"SDO block transfer: 32 bytes", 32U, kPatternSeed ^ 0x01U},
            {"SDO block transfer: 900 bytes", 900U, kPatternSeed ^ 0x02U},
            {"SDO block transfer: 1024 bytes", 1024U, kPatternSeed ^ 0x03U},
            {"SDO block transfer: 1025 bytes", 1025U, kPatternSeed ^ 0x04U},
            {"SDO block transfer: 2048 bytes", 2048U, kPatternSeed ^ 0x05U},
        };

        for (const auto& test_case : functional_cases) {
            if (!runBlockRoundTrip(master, test_case.name, test_case.size,
                                   test_case.seed, channel_known)) {
                cases_passed = false;
                break;
            }
        }
        if (!cases_passed) {
            break;
        }
        if (!runIllegalAccessCase(master, channel_known)) {
            cases_passed = false;
            break;
        }
        if (!runTimeoutRecoveryCase(master, channel_known, node_stopped)) {
            cases_passed = false;
            break;
        }
        if (!runClientAbortCase(master, channel_known, node_stopped)) {
            cases_passed = false;
            break;
        }
        if (!runExpeditedRegression(master, channel_known)) {
            cases_passed = false;
            break;
        }
        if (!runSegmentedRegression(master, channel_known)) {
            cases_passed = false;
            break;
        }
        if (!runStressCase(master, channel_known)) {
            cases_passed = false;
            break;
        }
    } while (false);

    /* NMT restoration is independent of SDO callback certainty and is needed
     * before any SDO cleanup can be considered safe. */
    if (node_stopped) {
        if (restorePreOperational(master)) {
            node_stopped = false;
        } else {
            cases_passed = false;
        }
    }

    bool cleanup_passed = false;
    if (channel_known && !node_stopped) {
        cleanup_passed = restoreOriginalPayload(master, original, channel_known);
    } else {
        spdlog::error(
            "SDO server block-transfer validation skips SDO cleanup because callback or NMT completion state is unknown");
    }

    if (!cases_passed || !cleanup_passed) {
        return 1;
    }

    spdlog::info("SDO server block-transfer validation passed");
    return 0;
}
