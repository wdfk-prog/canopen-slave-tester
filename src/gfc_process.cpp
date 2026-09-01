/**
 * @file
 * @brief Implements Global Fail-safe Command (GFC) protocol validation.
 */

#include "gfc_process.h"

#include "canopen_config.h"
#include "canopen_master.h"
#include "canopen_nmt.h"
#include "canopen_sdo.h"
#include "nmt_heartbeat.h"

#include <lely/can/msg.h>
#include <lely/io2/linux/can.hpp>

#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

namespace {

/** Standard CANopen Safety Global Fail-safe Command CAN-ID. */
constexpr std::uint32_t kGfcCanId = 0x001U;
/** Standard GFC parameter object. */
constexpr std::uint16_t kGfcParameterIndex = 0x1300U;
/** Demo/test GFC diagnostic and producer-control record. */
constexpr std::uint16_t kGfcDiagnosticIndex = 0x2302U;
/** Scalar sub-index. */
constexpr std::uint8_t kScalarSubindex = 0x00U;
/** Number of accepted GFC consumer callbacks. */
constexpr std::uint8_t kGfcRxCountSubindex = 0x01U;
/** Sticky indication that a valid GFC requested safe-state handling. */
constexpr std::uint8_t kGfcSafeRequestedSubindex = 0x02U;
/** Producer request sequence written by the Host. */
constexpr std::uint8_t kGfcProducerRequestSubindex = 0x03U;
/** Producer completion sequence published by the MCU mainline. */
constexpr std::uint8_t kGfcProducerCompleteSubindex = 0x04U;
/** Return value from the most recent MCU CO_GFCsend() call. */
constexpr std::uint8_t kGfcProducerResultSubindex = 0x05U;

/** Short protocol timeout for GFC integration validation SDO transactions. */
constexpr std::uint32_t kSdoTimeoutMs = 500U;
/** Local callback completion margin after the protocol timeout. */
constexpr std::uint32_t kSdoCompletionMarginMs = 100U;
/** Maximum wait for one expected GFC consumer callback. */
constexpr std::uint32_t kConsumerObservationTimeoutMs = 1500U;
/** Observation window used to prove an invalid frame does not trigger GFC. */
constexpr std::uint32_t kNegativeObservationMs = 300U;
/** Maximum wait for one MCU producer request to complete. */
constexpr std::uint32_t kProducerCompletionTimeoutMs = 1500U;
/** Maximum wait for an expected MCU-produced GFC frame. */
constexpr std::uint32_t kWireCaptureTimeoutMs = 1500U;
/** Short wire window used to prove a disabled producer emitted no GFC. */
constexpr std::uint32_t kNoWireFrameObservationMs = 300U;
/** Poll interval for MCU diagnostic state. */
constexpr std::uint32_t kDiagnosticPollIntervalMs = 20U;
/** Bounded synchronous wire write timeout. */
constexpr int kWireWriteTimeoutMs = 500;

/** Minimal producer/consumer state used by the Host test logic. */
struct GfcDiagnostic {
    std::uint32_t rx_count = 0U;
    std::uint8_t safe_requested = 0U;
    std::uint32_t producer_request_seq = 0U;
    std::uint32_t producer_complete_seq = 0U;
    std::int32_t producer_result = 0;
};

/** Runtime state required for best-effort cleanup after Reset Communication. */
struct GfcRuntimeState {
    std::uint8_t original_valid = 0U;
    bool original_valid_saved = false;
    bool reset_communication_issued = false;
    bool remote_operational_restored = false;
};

/** Result of one bounded wire observation. */
enum class WireWaitResult {
    FRAME, /**< A CAN-ID 0x001 frame was captured. */
    TIMEOUT, /**< The observation window expired without CAN-ID 0x001. */
    ERROR, /**< The dedicated CAN channel reported an I/O error. */
};

/** One captured fixed-ID frame with the kernel receive timestamp. */
struct GfcWireFrame {
    can_msg message = CAN_MSG_INIT;
    std::chrono::nanoseconds timestamp{0};
};

/**
 * @brief Process-local fixed GFC wire fixture on a dedicated Lely CanChannel.
 */
class GfcWireFixture {
public:
    explicit GfcWireFixture(lely::io::CanChannel& channel) noexcept
        : channel_(channel)
    {
    }

    /**
     * @brief Remove queued traffic before beginning a new wire assertion.
     *
     * @return true when the queue was drained without a channel error.
     */
    bool drain() noexcept
    {
        for (;;) {
            can_msg message = CAN_MSG_INIT;
            std::chrono::nanoseconds timestamp{0};
            std::error_code error;
            const int result = channel_.read(
                &message, nullptr, &timestamp, 0, error);
            if (result < 0) {
                if (isWouldBlock(error)) {
                    return true;
                }
                spdlog::error("GFC protocol validation wire drain failed: {}", error.message());
                return false;
            }
            if (result == 0) {
                return true;
            }
        }
    }

    /**
     * @brief Send the fixed GFC CAN-ID with a caller-selected DLC.
     *
     * DLC 0 is the valid GFC wire format. GFC integration validation uses DLC 1 only for the explicit
     * malformed-length negative case; arbitrary CAN-ID/payload transmission is
     * intentionally not exposed.
     *
     * @param dlc CAN payload length; only 0 or 1 is accepted by this fixture.
     * @return true when the CAN write completes successfully.
     */
    bool send(std::uint8_t dlc) noexcept
    {
        if (dlc > 1U) {
            spdlog::error("GFC protocol validation fixture rejects unsupported DLC {}",
                          static_cast<unsigned int>(dlc));
            return false;
        }

        can_msg message = CAN_MSG_INIT;
        message.id = kGfcCanId;
        message.flags = 0U;
        message.len = dlc;
        if (dlc == 1U) {
            message.data[0] = 0U;
        }

        std::error_code error;
        channel_.write(message, kWireWriteTimeoutMs, error);
        if (error) {
            spdlog::error("GFC protocol validation wire send failed: {}", error.message());
            return false;
        }
        return true;
    }

    /**
     * @brief Wait for the next CAN-ID 0x001 frame, ignoring unrelated traffic.
     *
     * @param timeout Maximum observation time.
     * @param frame Receives the matching frame and timestamp.
     * @return FRAME when CAN-ID 0x001 is observed, TIMEOUT when the window
     *         expires, or ERROR when the dedicated channel fails.
     */
    WireWaitResult waitForFrame(std::chrono::milliseconds timeout,
                                GfcWireFrame& frame) noexcept
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                break;
            }
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now);
            if (remaining.count() <= 0) {
                remaining = std::chrono::milliseconds(1);
            }

            can_msg message = CAN_MSG_INIT;
            std::chrono::nanoseconds timestamp{0};
            std::error_code error;
            const int result = channel_.read(
                &message, nullptr, &timestamp,
                boundedTimeoutMs(remaining), error);
            if (result < 0) {
                if (isWouldBlock(error)) {
                    break;
                }
                spdlog::error("GFC protocol validation wire capture failed: {}", error.message());
                return WireWaitResult::ERROR;
            }
            if (result == 0) {
                continue;
            }
            if (message.id != kGfcCanId) {
                continue;
            }

            frame.message = message;
            frame.timestamp = timestamp;
            return WireWaitResult::FRAME;
        } while (std::chrono::steady_clock::now() < deadline);

        return WireWaitResult::TIMEOUT;
    }

private:
    /** Return true for the timeout/nonblocking-empty error used by CanChannel. */
    static bool isWouldBlock(const std::error_code& error) noexcept
    {
        return error.value() == EAGAIN || error.value() == EWOULDBLOCK;
    }

    /** Clamp a chrono timeout to the signed millisecond API accepted by Lely. */
    static int boundedTimeoutMs(std::chrono::milliseconds timeout) noexcept
    {
        const auto maximum = static_cast<long long>(
            std::numeric_limits<int>::max());
        const long long value = timeout.count();
        if (value <= 0) {
            return 0;
        }
        if (value > maximum) {
            return std::numeric_limits<int>::max();
        }
        return static_cast<int>(value);
    }

    lely::io::CanChannel& channel_; /**< Dedicated process-local CAN channel. */
};

/**
 * @brief Read one GFC integration validation remote object using the process-local SDO budget.
 */
template <class T>
bool readRemote(lely::canopen::AsyncMaster& master,
                std::uint16_t index, std::uint8_t subindex,
                T& value, const char* label)
{
    if (readRemoteSdo<T>(
            master, CANOPEN_SLAVE_NODE_ID, index, subindex, value,
            std::chrono::milliseconds(kSdoTimeoutMs),
            std::chrono::milliseconds(kSdoCompletionMarginMs))
        != SdoOperationResult::SUCCESS) {
        spdlog::error("GFC protocol validation remote read failed: {}", label);
        return false;
    }
    return true;
}

/**
 * @brief Write one GFC integration validation remote object using the process-local SDO budget.
 */
template <class T>
bool writeRemote(lely::canopen::AsyncMaster& master,
                 std::uint16_t index, std::uint8_t subindex,
                 T value, const char* label)
{
    if (writeRemoteSdo<T>(
            master, CANOPEN_SLAVE_NODE_ID, index, subindex, value,
            std::chrono::milliseconds(kSdoTimeoutMs),
            std::chrono::milliseconds(kSdoCompletionMarginMs))
        != SdoOperationResult::SUCCESS) {
        spdlog::error("GFC protocol validation remote write failed: {}", label);
        return false;
    }
    return true;
}

/**
 * @brief Require the remote GFC parameter to reject value 2 with PARAM_VAL.
 *
 * A local submission failure, local wait timeout, protocol timeout, or another
 * SDO abort does not prove the MCU validated the GFC parameter correctly.
 */
bool requireInvalidGfcParameterRejected(lely::canopen::AsyncMaster& master)
{
    struct InvalidWriteState {
        std::mutex mutex;
        std::condition_variable condition;
        bool completed = false;
        std::error_code error;
    };

    const auto state = std::make_shared<InvalidWriteState>();
    std::error_code submit_error;
    master.SubmitWrite(
        master.GetExecutor(), CANOPEN_SLAVE_NODE_ID, kGfcParameterIndex,
        kScalarSubindex, static_cast<std::uint8_t>(2U),
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
        spdlog::error(
            "GFC protocol validation invalid 0x1300 write could not be submitted: {}",
            submit_error.message());
        return false;
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(
            lock,
            std::chrono::milliseconds(
                kSdoTimeoutMs + kSdoCompletionMarginMs),
            [state]() { return state->completed; })) {
        spdlog::error(
            "GFC protocol validation invalid 0x1300 write completion timed out; "
            "remote transaction state is unknown");
        return false;
    }

    if (!state->error) {
        spdlog::error("GFC protocol validation invalid 0x1300 value 2 was unexpectedly accepted");
        return false;
    }

    const lely::canopen::SdoErrc abort = lely::canopen::sdo_errc(state->error);
    if (abort != lely::canopen::SdoErrc::PARAM_VAL) {
        spdlog::error(
            "GFC protocol validation invalid 0x1300 returned wrong SDO abort: "
            "expected=0x06090030 actual=0x{:08x} ({})",
            static_cast<std::uint32_t>(abort), state->error.message());
        return false;
    }

    spdlog::info(
        "GFC protocol validation invalid 0x1300 value was rejected with PARAM_VAL 0x06090030");
    return true;
}

/**
 * @brief Read the complete MCU GFC diagnostic/control state.
 */
bool readDiagnostic(lely::canopen::AsyncMaster& master,
                    GfcDiagnostic& diagnostic)
{
    return readRemote(master, kGfcDiagnosticIndex, kGfcRxCountSubindex,
                      diagnostic.rx_count, "0x2302 GFC rx_count")
           && readRemote(master, kGfcDiagnosticIndex,
                         kGfcSafeRequestedSubindex,
                         diagnostic.safe_requested,
                         "0x2302 GFC safe_requested")
           && readRemote(master, kGfcDiagnosticIndex,
                         kGfcProducerRequestSubindex,
                         diagnostic.producer_request_seq,
                         "0x2302 GFC producer_request_seq")
           && readRemote(master, kGfcDiagnosticIndex,
                         kGfcProducerCompleteSubindex,
                         diagnostic.producer_complete_seq,
                         "0x2302 GFC producer_complete_seq")
           && readRemote(master, kGfcDiagnosticIndex,
                         kGfcProducerResultSubindex,
                         diagnostic.producer_result,
                         "0x2302 GFC producer_result");
}

/**
 * @brief Poll until exactly one additional valid GFC callback is published.
 */
bool waitForRxIncrement(lely::canopen::AsyncMaster& master,
                        std::uint32_t baseline,
                        std::uint32_t& observed)
{
    const std::uint32_t expected = baseline + 1U;
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(
                              kConsumerObservationTimeoutMs);
    do {
        std::uint32_t count = 0U;
        if (!readRemote(master, kGfcDiagnosticIndex, kGfcRxCountSubindex,
                        count, "0x2302 GFC rx_count poll")) {
            return false;
        }
        if (count == expected) {
            observed = count;
            return true;
        }
        if (count != baseline) {
            spdlog::error(
                "GFC protocol validation unexpected GFC callback count: baseline={} expected={} actual={}",
                baseline, expected, count);
            return false;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kDiagnosticPollIntervalMs));
    } while (std::chrono::steady_clock::now() < deadline);

    spdlog::error("GFC protocol validation callback timed out: baseline={} expected={}",
                  baseline, expected);
    return false;
}

/**
 * @brief Prove no GFC callback is published during a bounded negative window.
 */
bool requireRxUnchanged(lely::canopen::AsyncMaster& master,
                        std::uint32_t baseline,
                        std::chrono::milliseconds observation)
{
    const auto deadline = std::chrono::steady_clock::now() + observation;
    do {
        std::uint32_t count = 0U;
        if (!readRemote(master, kGfcDiagnosticIndex, kGfcRxCountSubindex,
                        count, "0x2302 GFC negative rx_count poll")) {
            return false;
        }
        if (count != baseline) {
            spdlog::error(
                "GFC protocol validation invalid/disabled GFC changed callback count: baseline={} actual={}",
                baseline, count);
            return false;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kDiagnosticPollIntervalMs));
    } while (std::chrono::steady_clock::now() < deadline);
    return true;
}

/**
 * @brief Emit one valid fixed-wire GFC and verify the MCU consumer diagnostic.
 */
bool sendAndValidateConsumer(lely::canopen::AsyncMaster& master,
                             GfcWireFixture& fixture,
                             std::uint32_t baseline,
                             std::uint32_t& observed,
                             const char* phase)
{
    if (!fixture.send(0U)) {
        return false;
    }
    if (!waitForRxIncrement(master, baseline, observed)) {
        spdlog::error("{} did not observe one valid GFC callback", phase);
        return false;
    }

    std::uint8_t safe_requested = 0U;
    if (!readRemote(master, kGfcDiagnosticIndex, kGfcSafeRequestedSubindex,
                    safe_requested, "0x2302 GFC safe_requested")) {
        return false;
    }
    if (safe_requested != 1U) {
        spdlog::error("{} safe_requested mismatch: expected=1 actual={}",
                      phase, static_cast<unsigned int>(safe_requested));
        return false;
    }

    spdlog::info("{} passed: rx_count={}", phase, observed);
    return true;
}

/**
 * @brief Wait for one producer request to reach the MCU mainline completion point.
 */
bool waitForProducerCompletion(lely::canopen::AsyncMaster& master,
                               std::uint32_t request_seq,
                               std::int32_t& producer_result)
{
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(
                              kProducerCompletionTimeoutMs);
    do {
        std::uint32_t complete_seq = 0U;
        if (!readRemote(master, kGfcDiagnosticIndex,
                        kGfcProducerCompleteSubindex, complete_seq,
                        "0x2302 GFC producer_complete_seq poll")) {
            return false;
        }
        if (complete_seq == request_seq) {
            if (!readRemote(master, kGfcDiagnosticIndex,
                            kGfcProducerResultSubindex, producer_result,
                            "0x2302 GFC producer_result")) {
                return false;
            }

            std::uint32_t confirm_seq = 0U;
            if (!readRemote(master, kGfcDiagnosticIndex,
                            kGfcProducerCompleteSubindex, confirm_seq,
                            "0x2302 GFC producer_complete_seq confirm")) {
                return false;
            }
            if (confirm_seq != request_seq) {
                spdlog::error(
                    "GFC protocol validation producer completion changed during result read: expected={} actual={}",
                    request_seq, confirm_seq);
                return false;
            }
            return true;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kDiagnosticPollIntervalMs));
    } while (std::chrono::steady_clock::now() < deadline);

    spdlog::error("GFC protocol validation producer request timed out: request_seq={}",
                  request_seq);
    return false;
}

/**
 * @brief Trigger exactly one MCU mainline CO_GFCsend() attempt.
 */
bool triggerProducer(lely::canopen::AsyncMaster& master,
                     std::uint32_t& request_seq,
                     std::int32_t& producer_result)
{
    request_seq += 1U;
    if (!writeRemote(master, kGfcDiagnosticIndex,
                     kGfcProducerRequestSubindex, request_seq,
                     "0x2302 GFC producer_request_seq")) {
        return false;
    }
    return waitForProducerCompletion(master, request_seq, producer_result);
}

/**
 * @brief Validate that a captured producer frame is the exact standard GFC format.
 */
bool validateProducerFrame(const GfcWireFrame& frame)
{
    const can_msg& message = frame.message;
    if (message.id != kGfcCanId || message.flags != 0U || message.len != 0U) {
        spdlog::error(
            "GFC protocol validation producer wire mismatch: id=0x{:x} flags=0x{:x} dlc={}",
            static_cast<unsigned int>(message.id),
            static_cast<unsigned int>(message.flags),
            static_cast<unsigned int>(message.len));
        return false;
    }

    spdlog::info(
        "GFC protocol validation producer wire frame passed: id=0x001 dlc=0 timestamp_ns={}",
        frame.timestamp.count());
    return true;
}

/**
 * @brief Run valid/invalid consumer tests and continuous delivery counting.
 */
bool validateConsumerCases(CanopenTestMaster& master,
                           GfcWireFixture& fixture,
                           GfcDiagnostic& diagnostic)
{
    if (!writeRemote(master, kGfcParameterIndex, kScalarSubindex,
                     static_cast<std::uint8_t>(1U), "0x1300 GFC valid=1")) {
        return false;
    }

    std::uint32_t observed = diagnostic.rx_count;
    if (!sendAndValidateConsumer(master, fixture, diagnostic.rx_count,
                                 observed, "GFC valid-consumer delivery")) {
        return false;
    }
    diagnostic.rx_count = observed;

    if (!writeRemote(master, kGfcParameterIndex, kScalarSubindex,
                     static_cast<std::uint8_t>(0U), "0x1300 GFC valid=0")) {
        return false;
    }
    if (!fixture.send(0U)
        || !requireRxUnchanged(
            master, diagnostic.rx_count,
            std::chrono::milliseconds(kNegativeObservationMs))) {
        spdlog::error("GFC disabled-consumer gate accepted a GFC while disabled");
        return false;
    }
    spdlog::info("GFC disabled-consumer gate passed: valid=0");

    /* Values greater than one are outside the standard GFC parameter domain.
     * Require CANopenNode's specific Invalid value for parameter SDO abort. */
    if (!requireInvalidGfcParameterRejected(master)) {
        return false;
    }

    std::uint8_t current_valid = 0xFFU;
    if (!readRemote(master, kGfcParameterIndex, kScalarSubindex,
                    current_valid, "0x1300 after invalid value")
        || current_valid != 0U) {
        spdlog::error(
            "GFC protocol validation invalid 0x1300 write changed the parameter: expected=0 actual={}",
            static_cast<unsigned int>(current_valid));
        return false;
    }
    spdlog::info("GFC protocol validation invalid 0x1300 value was rejected and baseline preserved");

    if (!writeRemote(master, kGfcParameterIndex, kScalarSubindex,
                     static_cast<std::uint8_t>(1U), "restore 0x1300 valid=1")) {
        return false;
    }
    if (!fixture.send(1U)
        || !requireRxUnchanged(
            master, diagnostic.rx_count,
            std::chrono::milliseconds(kNegativeObservationMs))) {
        spdlog::error("GFC invalid-DLC check accepted an invalid-DLC frame as GFC");
        return false;
    }
    spdlog::info("GFC invalid-DLC rejection passed");

    for (unsigned int i = 0U; i < 3U; ++i) {
        const std::uint32_t baseline = diagnostic.rx_count;
        if (!sendAndValidateConsumer(master, fixture, baseline,
                                     observed, "GFC continuous-delivery sample")) {
            return false;
        }
        diagnostic.rx_count = observed;
    }
    spdlog::info("GFC continuous-delivery count passed");
    return true;
}

/**
 * @brief Validate MCU GFC producer gating and exact wire format.
 */
bool validateProducerCases(CanopenTestMaster& master,
                           GfcWireFixture& fixture,
                           GfcDiagnostic& diagnostic)
{
    if (diagnostic.producer_request_seq != diagnostic.producer_complete_seq) {
        spdlog::error(
            "GFC protocol validation refuses producer test with pending request: request={} complete={}",
            diagnostic.producer_request_seq,
            diagnostic.producer_complete_seq);
        return false;
    }

    if (!writeRemote(master, kGfcParameterIndex, kScalarSubindex,
                     static_cast<std::uint8_t>(0U),
                     "0x1300 disable producer")) {
        return false;
    }
    if (!fixture.drain()) {
        return false;
    }

    std::int32_t producer_result = 0;
    std::uint32_t request_seq = diagnostic.producer_request_seq;
    if (!triggerProducer(master, request_seq, producer_result)) {
        return false;
    }

    GfcWireFrame unexpected;
    const WireWaitResult disabled_wait = fixture.waitForFrame(
        std::chrono::milliseconds(kNoWireFrameObservationMs), unexpected);
    if (disabled_wait == WireWaitResult::ERROR) {
        return false;
    }
    if (disabled_wait == WireWaitResult::FRAME) {
        spdlog::error(
            "GFC protocol validation disabled producer emitted CAN-ID 0x001: flags=0x{:x} dlc={}",
            static_cast<unsigned int>(unexpected.message.flags),
            static_cast<unsigned int>(unexpected.message.len));
        return false;
    }
    if (producer_result != 0) {
        spdlog::error(
            "GFC protocol validation disabled producer returned an unexpected CO_GFCsend result: {}",
            producer_result);
        return false;
    }
    diagnostic.producer_request_seq = request_seq;
    diagnostic.producer_complete_seq = request_seq;
    diagnostic.producer_result = producer_result;
    spdlog::info(
        "GFC protocol validation producer valid=0 gate passed: CO_GFCsend result={}",
        producer_result);

    if (!writeRemote(master, kGfcParameterIndex, kScalarSubindex,
                     static_cast<std::uint8_t>(1U),
                     "0x1300 enable producer")) {
        return false;
    }
    if (!fixture.drain()) {
        return false;
    }

    if (!triggerProducer(master, request_seq, producer_result)) {
        return false;
    }
    GfcWireFrame produced;
    const WireWaitResult producer_wait = fixture.waitForFrame(
        std::chrono::milliseconds(kWireCaptureTimeoutMs), produced);
    if (producer_wait == WireWaitResult::ERROR) {
        return false;
    }
    if (producer_wait == WireWaitResult::TIMEOUT) {
        spdlog::error("GFC producer wire-format check did not capture MCU-produced GFC");
        return false;
    }
    if (producer_result != 0) {
        spdlog::error(
            "GFC producer wire-format check MCU CO_GFCsend failed despite a completed request: result={}",
            producer_result);
        return false;
    }
    if (!validateProducerFrame(produced)) {
        return false;
    }

    diagnostic.producer_request_seq = request_seq;
    diagnostic.producer_complete_seq = request_seq;
    diagnostic.producer_result = producer_result;
    spdlog::info("GFC producer wire-format check passed: result={}",
                 producer_result);
    return true;
}

/**
 * @brief Reset MCU communication and prove the GFC consumer callback is rebound.
 */
bool validateResetRebind(CanopenTestMaster& master,
                         GfcWireFixture& fixture,
                         GfcRuntimeState& state,
                         GfcDiagnostic& diagnostic)
{
    const std::uint32_t pre_reset_count = diagnostic.rx_count;

    prepareBootWait();
    if (!issueNmtCommand(master, lely::canopen::NmtCommand::RESET_COMM,
                         CANOPEN_SLAVE_NODE_ID,
                         "GFC protocol validation Reset Communication")) {
        return false;
    }
    state.reset_communication_issued = true;
    state.remote_operational_restored = false;

    lely::canopen::NmtState boot_state = lely::canopen::NmtState::BOOTUP;
    if (!waitForBootCompletion(
            std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS), boot_state)) {
        spdlog::error("GFC reset/rebind verification Boot after Reset Communication timed out");
        return false;
    }
    state.remote_operational_restored =
        boot_state == lely::canopen::NmtState::START;

    GfcDiagnostic post_reset;
    if (!readDiagnostic(master, post_reset)) {
        return false;
    }
    if (post_reset.rx_count != pre_reset_count) {
        spdlog::error(
            "GFC reset/rebind verification rx_count changed across Reset Communication: before={} after={}",
            pre_reset_count, post_reset.rx_count);
        return false;
    }
    if (post_reset.producer_request_seq != post_reset.producer_complete_seq) {
        spdlog::error(
            "GFC reset/rebind verification producer request became pending across reset: request={} complete={}",
            post_reset.producer_request_seq,
            post_reset.producer_complete_seq);
        return false;
    }

    std::uint8_t valid = 0U;
    if (!readRemote(master, kGfcParameterIndex, kScalarSubindex,
                    valid, "0x1300 after Reset Communication")) {
        return false;
    }
    if (valid != 1U
        && !writeRemote(master, kGfcParameterIndex, kScalarSubindex,
                        static_cast<std::uint8_t>(1U),
                        "enable GFC after Reset Communication")) {
        return false;
    }

    std::uint32_t observed = post_reset.rx_count;
    if (!sendAndValidateConsumer(master, fixture, post_reset.rx_count,
                                 observed, "GFC reset/rebind verification reset callback rebind")) {
        return false;
    }
    post_reset.rx_count = observed;
    diagnostic = post_reset;

    if (!state.remote_operational_restored) {
        if (!issueNmtCommandAndWaitForState(
                master, lely::canopen::NmtCommand::START,
                CANOPEN_SLAVE_NODE_ID, lely::canopen::NmtState::START,
                std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS),
                "GFC protocol validation final NMT Start")) {
            spdlog::error("GFC reset/rebind verification could not restore MCU Operational state");
            return false;
        }
        state.remote_operational_restored = true;
    }

    spdlog::info("GFC reset/rebind verification Reset Communication callback rebind passed");
    return true;
}

/**
 * @brief Verify normal SDO service after GFC protocol activity.
 */
bool validateProtocolHealth(lely::canopen::AsyncMaster& master)
{
    std::uint32_t device_type = 0U;
    if (!readRemote(master, 0x1000U, kScalarSubindex,
                    device_type, "0x1000 device type health read")) {
        spdlog::error("GFC protocol-health regression ordinary SDO health check failed");
        return false;
    }
    spdlog::info("GFC protocol-health regression ordinary SDO health passed: 0x1000=0x{:08x}",
                 device_type);
    return true;
}

/**
 * @brief Settle a producer request that may still be pending after a failed wait.
 *
 * The request SDO can succeed before a later completion poll fails. Disable the
 * producer first so a late mainline execution cannot emit a new GFC, then wait
 * for the request/complete sequence pair to converge before restoring 0x1300.
 */
bool settlePendingProducer(CanopenTestMaster& master)
{
    /* Disable GFC before inspecting the sequence pair. If a prior request is
     * still pending, a late mainline execution must not emit another frame. */
    if (!writeRemote(master, kGfcParameterIndex, kScalarSubindex,
                     static_cast<std::uint8_t>(0U),
                     "disable GFC while settling producer request")) {
        return false;
    }

    std::uint32_t request_seq = 0U;
    std::uint32_t complete_seq = 0U;
    if (!readRemote(master, kGfcDiagnosticIndex, kGfcProducerRequestSubindex,
                    request_seq, "0x2302 cleanup producer_request_seq")
        || !readRemote(master, kGfcDiagnosticIndex,
                       kGfcProducerCompleteSubindex, complete_seq,
                       "0x2302 cleanup producer_complete_seq")) {
        return false;
    }
    if (request_seq == complete_seq) {
        return true;
    }

    spdlog::warn(
        "GFC protocol validation cleanup settling pending producer request: request={} complete={}",
        request_seq, complete_seq);

    std::int32_t producer_result = 0;
    if (!waitForProducerCompletion(master, request_seq, producer_result)) {
        spdlog::error(
            "GFC protocol validation cleanup could not settle pending producer request {}",
            request_seq);
        return false;
    }

    std::uint32_t confirmed_request = 0U;
    std::uint32_t confirmed_complete = 0U;
    if (!readRemote(master, kGfcDiagnosticIndex, kGfcProducerRequestSubindex,
                    confirmed_request, "0x2302 cleanup request confirm")
        || !readRemote(master, kGfcDiagnosticIndex,
                       kGfcProducerCompleteSubindex, confirmed_complete,
                       "0x2302 cleanup complete confirm")) {
        return false;
    }
    if (confirmed_request != request_seq
        || confirmed_complete != request_seq) {
        spdlog::error(
            "GFC protocol validation cleanup producer sequence did not converge: expected={} request={} complete={}",
            request_seq, confirmed_request, confirmed_complete);
        return false;
    }

    return true;
}

/**
 * @brief Restore remote configuration/state owned by GFC integration validation after success or failure.
 */
bool cleanupGfcValidation(CanopenTestMaster& master,
                          GfcRuntimeState& state)
{
    bool result = true;

    /* Try to restore Operational first after a reset, but continue with the
     * parameter restore even if NMT recovery fails so cleanup remains best-effort. */
    if (state.reset_communication_issued
        && !state.remote_operational_restored) {
        if (!issueNmtCommandAndWaitForState(
                master, lely::canopen::NmtCommand::START,
                CANOPEN_SLAVE_NODE_ID, lely::canopen::NmtState::START,
                std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS),
                "GFC protocol validation cleanup NMT Start")) {
            spdlog::error("GFC protocol validation cleanup could not restore MCU Operational state");
            result = false;
        } else {
            state.remote_operational_restored = true;
        }
    }

    if (state.original_valid_saved) {
        const bool producer_settled = settlePendingProducer(master);
        if (!producer_settled) {
            /* Do not re-enable a producer while a previous request may still
             * be pending. Leave the process dirty/failed instead. */
            spdlog::error(
                "GFC protocol validation cleanup leaves GFC disabled because producer state could not be settled");
            result = false;
        } else if (!writeRemote(master, kGfcParameterIndex, kScalarSubindex,
                                state.original_valid,
                                "restore original 0x1300 GFC parameter")) {
            result = false;
        } else {
            std::uint8_t restored = 0xFFU;
            if (!readRemote(master, kGfcParameterIndex, kScalarSubindex,
                            restored, "verify restored 0x1300")
                || restored != state.original_valid) {
                spdlog::error(
                    "GFC protocol validation cleanup 0x1300 restore mismatch: expected={} actual={}",
                    static_cast<unsigned int>(state.original_valid),
                    static_cast<unsigned int>(restored));
                result = false;
            }
        }
    }

    return result;
}

/**
 * @brief Execute the complete GFC protocol validation sequence.
 */
bool runGfcValidation(CanopenTestMaster& master,
                      GfcWireFixture& fixture,
                      GfcRuntimeState& state)
{
    if (!fixture.drain()) {
        return false;
    }

    std::uint8_t original_valid = 0U;
    if (!readRemote(master, kGfcParameterIndex, kScalarSubindex,
                    original_valid, "initial 0x1300 GFC parameter")) {
        return false;
    }
    if (original_valid > 1U) {
        spdlog::error("GFC protocol validation invalid initial 0x1300 value: {}",
                      static_cast<unsigned int>(original_valid));
        return false;
    }
    state.original_valid = original_valid;
    state.original_valid_saved = true;

    GfcDiagnostic diagnostic;
    if (!readDiagnostic(master, diagnostic)) {
        spdlog::error(
            "GFC protocol validation MCU diagnostic 0x2302 is unavailable; enable the GFC demo diagnostic firmware option");
        return false;
    }
    if (diagnostic.producer_request_seq != diagnostic.producer_complete_seq) {
        spdlog::error(
            "GFC protocol validation preflight found pending producer request: request={} complete={}",
            diagnostic.producer_request_seq,
            diagnostic.producer_complete_seq);
        return false;
    }

    spdlog::info(
        "GFC protocol validation preflight passed: 0x1300={} rx_count={} producer_seq={}",
        static_cast<unsigned int>(original_valid), diagnostic.rx_count,
        diagnostic.producer_complete_seq);

    if (!validateConsumerCases(master, fixture, diagnostic)
        || !validateProducerCases(master, fixture, diagnostic)
        || !validateResetRebind(master, fixture, state, diagnostic)
        || !validateProtocolHealth(master)) {
        return false;
    }

    return true;
}

} // namespace

int gfcProcess(CanopenTestMaster& master, lely::io::CanChannel& wire_channel)
{
    GfcWireFixture fixture(wire_channel);
    GfcRuntimeState state;

    int result = runGfcValidation(master, fixture, state) ? 0 : 1;
    if (!cleanupGfcValidation(master, state)) {
        result = 1;
    }

    if (result == 0) {
        spdlog::info(
            "GFC protocol validation consumer, producer, invalid-frame, continuous-count, reset-rebind, and SDO-health tests passed");
    }
    return result;
}
