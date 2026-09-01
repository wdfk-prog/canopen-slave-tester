/**
 * @file
 * @brief Implements TIME consumer validation.
 */

#include "time_process.h"

#include "canopen_config.h"
#include "canopen_nmt.h"
#include "canopen_sdo.h"
#include "nmt_heartbeat.h"

#include <lely/can/net.hpp>
#include <lely/can/msg.h>
#include <lely/coapp/master.hpp>
#include <lely/io2/can_net.h>
#include <lely/util/errnum.h>

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace {

/** OD 0x1012 configures the CANopen TIME consumer/producer role and CAN-ID. */
constexpr std::uint16_t kTimeCobIdIndex = 0x1012;
/** TIME COB-ID is a scalar object at sub-index zero. */
constexpr std::uint8_t kTimeCobIdSubindex = 0x00;
/** Demo diagnostic record populated by the MCU TIME application adapter. */
constexpr std::uint16_t kTimeDiagnosticIndex = 0x2300;
/** Number of syntactically valid TIME frames observed by the MCU receive callback. */
constexpr std::uint8_t kTimeRxCountSubindex = 0x01;
/** Application milliseconds after midnight from CO_TIME_t::ms. */
constexpr std::uint8_t kTimeMillisecondsSubindex = 0x02;
/** Application day count since 1984-01-01 from CO_TIME_t::days. */
constexpr std::uint8_t kTimeDaysSubindex = 0x03;

/** Bit 31 of 0x1012 enables TIME consumption. */
constexpr std::uint32_t kTimeConsumerMask = 0x80000000U;
/** Bit 30 of 0x1012 enables TIME production. */
constexpr std::uint32_t kTimeProducerMask = 0x40000000U;
/** Current CANopenNode target accepts only an 11-bit TIME CAN-ID. */
constexpr std::uint32_t kTimeCanIdMask = 0x000007FFU;
/** Bits 11..29 are rejected by the current CANopenNode 0x1012 write hook. */
constexpr std::uint32_t kTimeUnsupportedCobIdMask = 0x3FFFF800U;
/** A CANopen TIME frame contains four millisecond bytes and two day bytes. */
constexpr std::uint8_t kTimeMessageLength = 6U;
/** Maximum Classic CAN payload length used by the invalid-DLC checks. */
constexpr std::uint8_t kClassicCanMaxLength = 8U;

/** Milliseconds in one CANopen TIME day. */
constexpr std::uint64_t kMillisecondsPerDay = 86400000ULL;
/** Full wrap period of the 16-bit CANopen day counter. */
constexpr std::uint64_t kTimeCycleMilliseconds =
    65536ULL * kMillisecondsPerDay;
/** Additional host-side allowance for SDO scheduling after a TIME frame. */
constexpr std::uint32_t kAppliedTimeToleranceMs = 250U;
/** Delay used to observe normal internal TIME progression. */
constexpr std::uint32_t kContinuousWaitMs = 250U;
/** Allowed host/MCU scheduling difference during progression checks. */
constexpr std::uint32_t kContinuousToleranceMs = 120U;
/** Delay after invalid/disabled injections before reading diagnostics. */
constexpr std::uint32_t kNegativeObservationWaitMs = 120U;
/** Poll spacing while waiting for the MCU to publish a valid TIME RX count. */
constexpr std::uint32_t kDiagnosticPollMs = 20U;
/** Bound for waiting until one valid TIME injection is applied. */
constexpr std::uint32_t kDiagnosticApplyTimeoutMs = 1500U;
/** Retry count for a coherent diagnostic snapshot around a day rollover. */
constexpr unsigned int kDiagnosticSnapshotRetryCount = 3U;

using Clock = std::chrono::steady_clock;

/** Coherent application-level view of the MCU TIME consumer state. */
struct TimeDiagnostic {
    std::uint32_t rx_count = 0; /**< Valid DLC=6 TIME receive sequence counter. */
    std::uint32_t milliseconds = 0; /**< Milliseconds after midnight. */
    std::uint16_t days = 0; /**< Days since 1984-01-01. */
    Clock::time_point sampled_at{}; /**< Host time immediately after the ms SDO completes. */
};

/**
 * @brief Convert a TIME value to one position in its modulo time cycle.
 */
std::uint64_t timePosition(std::uint32_t milliseconds, std::uint16_t days)
{
    return static_cast<std::uint64_t>(days) * kMillisecondsPerDay
           + milliseconds;
}

/**
 * @brief Compute forward elapsed milliseconds across the 16-bit day wrap.
 */
std::uint64_t forwardTimeDelta(std::uint64_t from, std::uint64_t to)
{
    return to >= from ? to - from : kTimeCycleMilliseconds - from + to;
}

/**
 * @brief Read only the valid TIME RX count for low-cost polling.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param rx_count Receives diagnostic 0x2300:01.
 * @param completion_wait_timed_out Set when the local SDO completion wait expires.
 * @return true on a successful upload; otherwise false.
 */
bool readRxCount(lely::canopen::AsyncMaster& master,
                 std::uint32_t& rx_count,
                 bool& completion_wait_timed_out)
{
    const SdoOperationResult result = readRemoteSdo(
        master, CANOPEN_SLAVE_NODE_ID, kTimeDiagnosticIndex,
        kTimeRxCountSubindex, rx_count);
    if (result == SdoOperationResult::WAIT_TIMEOUT) {
        completion_wait_timed_out = true;
    }
    return result == SdoOperationResult::SUCCESS;
}

/**
 * @brief Read one coherent TIME diagnostic snapshot from the MCU.
 *
 * The day field is sampled around the millisecond upload and the valid RX count
 * is sampled around the whole sequence. A rollover or new valid TIME reception
 * during the SDO sequence causes a bounded retry instead of publishing a mixed
 * snapshot.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param diagnostic Receives a coherent application TIME snapshot.
 * @param completion_wait_timed_out Set when any local SDO wait expires.
 * @return true on a coherent snapshot; otherwise false.
 */
bool readTimeDiagnostic(lely::canopen::AsyncMaster& master,
                        TimeDiagnostic& diagnostic,
                        bool& completion_wait_timed_out)
{
    for (unsigned int attempt = 0; attempt < kDiagnosticSnapshotRetryCount;
         ++attempt) {
        std::uint32_t count_before = 0;
        std::uint32_t count_after = 0;
        std::uint32_t milliseconds = 0;
        std::uint16_t days_before = 0;
        std::uint16_t days_after = 0;

        if (!readRxCount(master, count_before,
                         completion_wait_timed_out)) {
            return false;
        }

        SdoOperationResult result = readRemoteSdo(
            master, CANOPEN_SLAVE_NODE_ID, kTimeDiagnosticIndex,
            kTimeDaysSubindex, days_before);
        if (result == SdoOperationResult::WAIT_TIMEOUT) {
            completion_wait_timed_out = true;
            return false;
        }
        if (result != SdoOperationResult::SUCCESS) {
            return false;
        }

        result = readRemoteSdo(master, CANOPEN_SLAVE_NODE_ID,
                               kTimeDiagnosticIndex,
                               kTimeMillisecondsSubindex, milliseconds);
        const Clock::time_point milliseconds_sampled_at = Clock::now();
        if (result == SdoOperationResult::WAIT_TIMEOUT) {
            completion_wait_timed_out = true;
            return false;
        }
        if (result != SdoOperationResult::SUCCESS) {
            return false;
        }

        result = readRemoteSdo(master, CANOPEN_SLAVE_NODE_ID,
                               kTimeDiagnosticIndex, kTimeDaysSubindex,
                               days_after);
        if (result == SdoOperationResult::WAIT_TIMEOUT) {
            completion_wait_timed_out = true;
            return false;
        }
        if (result != SdoOperationResult::SUCCESS) {
            return false;
        }

        if (!readRxCount(master, count_after,
                         completion_wait_timed_out)) {
            return false;
        }

        if (count_before == count_after && days_before == days_after) {
            if (milliseconds >= kMillisecondsPerDay) {
                spdlog::error(
                    "TIME consumer validation diagnostic milliseconds out of range: {}",
                    milliseconds);
                return false;
            }
            diagnostic.rx_count = count_after;
            diagnostic.milliseconds = milliseconds;
            diagnostic.days = days_after;
            diagnostic.sampled_at = milliseconds_sampled_at;
            return true;
        }

        spdlog::debug(
            "TIME consumer validation diagnostic snapshot changed during SDO reads; retrying "
            "(attempt={})",
            attempt + 1U);
    }

    spdlog::error(
        "TIME consumer validation unable to obtain a coherent TIME diagnostic snapshot");
    return false;
}

/**
 * @brief Send one exact Classic CAN TIME frame through the existing Lely net.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param can_id Standard 11-bit TIME CAN-ID.
 * @param dlc Classic CAN data length to place on the bus.
 * @param milliseconds TIME milliseconds encoded in bytes 0..3.
 * @param days TIME day count encoded in bytes 4..5.
 * @return true when Lely accepts the frame into its transmit queue.
 */
bool sendTimeFrame(lely::canopen::AsyncMaster& master, std::uint32_t can_id,
                   std::uint8_t dlc, std::uint32_t milliseconds,
                   std::uint16_t days)
{
    if (can_id > kTimeCanIdMask || dlc > kClassicCanMaxLength) {
        spdlog::error(
            "TIME consumer validation invalid TIME injection parameters: can_id=0x{:x} dlc={}",
            can_id, static_cast<unsigned int>(dlc));
        return false;
    }

    struct can_msg message = CAN_MSG_INIT;
    message.id = can_id;
    message.flags = 0U;
    message.len = dlc;
    message.data[0] = static_cast<std::uint8_t>(milliseconds & 0xFFU);
    message.data[1] =
        static_cast<std::uint8_t>((milliseconds >> 8U) & 0xFFU);
    message.data[2] =
        static_cast<std::uint8_t>((milliseconds >> 16U) & 0xFFU);
    message.data[3] =
        static_cast<std::uint8_t>((milliseconds >> 24U) & 0xFFU);
    message.data[4] = static_cast<std::uint8_t>(days & 0xFFU);
    message.data[5] = static_cast<std::uint8_t>((days >> 8U) & 0xFFU);
    /* The extra byte is deliberately nonzero for the DLC=7 negative case. */
    message.data[6] = 0xA5U;

    /* AsyncMaster publicly exposes io_can_net_t but protects its internal CAN
     * network. The C API provides a supported locked accessor for exact frame
     * injection while retaining the existing Lely transmit queue. */
    io_can_net_t* io_net = master;
    if (io_can_net_lock(io_net) == -1) {
        spdlog::error("TIME consumer validation unable to lock Lely CAN network: errc={}",
                      get_errc());
        return false;
    }

    struct __can_net* raw_net = io_can_net_get_net(io_net);
    int send_result = -1;
    int send_errc = 0;
    if (raw_net != nullptr) {
        /* Lely's C++ CANNet wrapper overlays the same opaque __can_net object;
         * the conversion is required because can_net_t is that wrapper type in
         * C++ builds while io_can_net_get_net() intentionally returns the C
         * opaque pointer. */
        auto* can_net = reinterpret_cast<can_net_t*>(raw_net);
        send_result = can_net_send(can_net, &message);
        if (send_result == -1) {
            send_errc = get_errc();
        }
    }

    const int unlock_result = io_can_net_unlock(io_net);
    const int unlock_errc = unlock_result == -1 ? get_errc() : 0;

    if (raw_net == nullptr) {
        spdlog::error("TIME consumer validation Lely CAN network internal interface is null");
        return false;
    }
    if (send_result == -1) {
        spdlog::error(
            "TIME consumer validation unable to queue TIME frame: can_id=0x{:03x} dlc={} errc={}",
            can_id, static_cast<unsigned int>(dlc), send_errc);
        return false;
    }
    if (unlock_result == -1) {
        spdlog::error("TIME consumer validation unable to unlock Lely CAN network: errc={}",
                      unlock_errc);
        return false;
    }

    spdlog::info(
        "TIME consumer validation frame queued: can_id=0x{:03x} dlc={} days={} ms={}",
        can_id, static_cast<unsigned int>(dlc),
        static_cast<unsigned int>(days), milliseconds);
    return true;
}

/**
 * @brief Wait until one exact valid TIME RX count increment is published.
 */
bool waitForRxCount(lely::canopen::AsyncMaster& master,
                    std::uint32_t previous_count,
                    bool& completion_wait_timed_out)
{
    const std::uint32_t expected_count = previous_count + 1U;
    const Clock::time_point deadline =
        Clock::now()
        + std::chrono::milliseconds(kDiagnosticApplyTimeoutMs);

    while (Clock::now() < deadline) {
        std::uint32_t current_count = 0;
        if (!readRxCount(master, current_count,
                         completion_wait_timed_out)) {
            return false;
        }
        if (current_count != previous_count) {
            if (current_count != expected_count) {
                spdlog::error(
                    "TIME consumer validation valid TIME RX count mismatch: expected={} actual={}",
                    expected_count, current_count);
                return false;
            }
            return true;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kDiagnosticPollMs));
    }

    spdlog::error(
        "TIME consumer validation valid TIME reception timed out: rx_count remained {}",
        previous_count);
    return false;
}

/**
 * @brief Wait until the application TIME matches one injected timestamp.
 *
 * The receive callback may publish 0x2300:01 before the RTOS main loop reaches
 * CO_TIME_process(). Keep the receive-count proof and application-state proof
 * independent so normal scheduling does not create a false failure.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param expected_rx_count Exact receive count after the injected frame.
 * @param sent_milliseconds Injected milliseconds after midnight.
 * @param sent_days Injected day count.
 * @param send_time Host timestamp immediately before the CAN frame was queued.
 * @param diagnostic Receives the matched diagnostic snapshot.
 * @param completion_wait_timed_out Set when any local SDO wait expires.
 * @return true when the applied time matches before the bounded deadline.
 */
bool waitForAppliedTime(lely::canopen::AsyncMaster& master,
                        std::uint32_t expected_rx_count,
                        std::uint32_t sent_milliseconds,
                        std::uint16_t sent_days,
                        Clock::time_point send_time,
                        TimeDiagnostic& diagnostic,
                        bool& completion_wait_timed_out)
{
    const std::uint64_t sent = timePosition(sent_milliseconds, sent_days);
    const Clock::time_point deadline =
        Clock::now()
        + std::chrono::milliseconds(kDiagnosticApplyTimeoutMs);
    TimeDiagnostic last{};
    bool have_snapshot = false;

    while (Clock::now() < deadline) {
        if (!readTimeDiagnostic(master, last, completion_wait_timed_out)) {
            return false;
        }
        have_snapshot = true;

        if (last.rx_count != expected_rx_count) {
            spdlog::error(
                "TIME consumer validation valid TIME RX count changed while waiting for application: "
                "expected={} actual={}",
                expected_rx_count, last.rx_count);
            return false;
        }

        const auto host_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                last.sampled_at - send_time);
        const std::uint64_t observed =
            timePosition(last.milliseconds, last.days);
        const std::uint64_t remote_elapsed =
            forwardTimeDelta(sent, observed);
        const std::uint64_t maximum_elapsed =
            static_cast<std::uint64_t>(host_elapsed.count())
            + kAppliedTimeToleranceMs;

        if (remote_elapsed <= maximum_elapsed) {
            diagnostic = last;
            return true;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(kDiagnosticPollMs));
    }

    if (have_snapshot) {
        const auto host_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                last.sampled_at - send_time);
        const std::uint64_t observed =
            timePosition(last.milliseconds, last.days);
        const std::uint64_t remote_elapsed =
            forwardTimeDelta(sent, observed);
        spdlog::error(
            "TIME consumer validation applied TIME did not converge before timeout: sent days={} "
            "ms={} observed days={} ms={} remote_elapsed={} ms "
            "host_elapsed={} ms",
            static_cast<unsigned int>(sent_days), sent_milliseconds,
            static_cast<unsigned int>(last.days), last.milliseconds,
            remote_elapsed, host_elapsed.count());
    } else {
        spdlog::error("TIME consumer validation applied TIME wait ended without a diagnostic snapshot");
    }
    return false;
}

/**
 * @brief Inject one valid TIME and prove that the MCU applies it exactly once.
 */
bool injectAndVerifyTime(lely::canopen::AsyncMaster& master,
                         std::uint32_t can_id, std::uint32_t milliseconds,
                         std::uint16_t days,
                         bool& completion_wait_timed_out,
                         TimeDiagnostic* observed_diagnostic = nullptr)
{
    std::uint32_t previous_count = 0;
    if (!readRxCount(master, previous_count,
                     completion_wait_timed_out)) {
        return false;
    }

    const Clock::time_point send_time = Clock::now();
    if (!sendTimeFrame(master, can_id, kTimeMessageLength,
                       milliseconds, days)) {
        return false;
    }
    if (!waitForRxCount(master, previous_count,
                        completion_wait_timed_out)) {
        return false;
    }

    TimeDiagnostic diagnostic{};
    if (!waitForAppliedTime(master, previous_count + 1U, milliseconds, days,
                            send_time, diagnostic,
                            completion_wait_timed_out)) {
        return false;
    }

    if (observed_diagnostic != nullptr) {
        *observed_diagnostic = diagnostic;
    }
    return true;
}

/**
 * @brief Verify normal internal TIME progression without a new timestamp.
 */
bool validateContinuousAdvance(const TimeDiagnostic& before,
                               const TimeDiagnostic& after,
                               std::uint32_t expected_rx_count_delta = 0U)
{
    const std::uint32_t rx_count_delta = after.rx_count - before.rx_count;
    if (rx_count_delta != expected_rx_count_delta) {
        spdlog::error(
            "TIME consumer validation valid TIME RX count delta mismatch: expected={} actual={} "
            "before={} after={}",
            expected_rx_count_delta, rx_count_delta, before.rx_count,
            after.rx_count);
        return false;
    }

    const std::uint64_t before_position =
        timePosition(before.milliseconds, before.days);
    const std::uint64_t after_position =
        timePosition(after.milliseconds, after.days);
    const std::uint64_t remote_elapsed =
        forwardTimeDelta(before_position, after_position);
    const auto host_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            after.sampled_at - before.sampled_at);
    const std::uint64_t expected =
        static_cast<std::uint64_t>(host_elapsed.count());
    const std::uint64_t minimum =
        expected > kContinuousToleranceMs
            ? expected - kContinuousToleranceMs
            : 0U;
    const std::uint64_t maximum = expected + kContinuousToleranceMs;

    if (remote_elapsed < minimum || remote_elapsed > maximum) {
        spdlog::error(
            "TIME consumer validation progression out of tolerance: remote={} ms host={} ms "
            "allowed=[{}, {}] ms",
            remote_elapsed, host_elapsed.count(), minimum, maximum);
        return false;
    }
    return true;
}

/**
 * @brief Write and strictly read back slave 0x1012.
 */
bool writeAndVerifyTimeCobId(lely::canopen::AsyncMaster& master,
                             std::uint32_t value,
                             bool& completion_wait_timed_out)
{
    const SdoOperationResult write_result = writeRemoteSdo(
        master, CANOPEN_SLAVE_NODE_ID, kTimeCobIdIndex,
        kTimeCobIdSubindex, value);
    if (write_result == SdoOperationResult::WAIT_TIMEOUT) {
        completion_wait_timed_out = true;
        return false;
    }
    if (write_result != SdoOperationResult::SUCCESS) {
        return false;
    }

    std::uint32_t read_back = 0;
    const SdoOperationResult read_result = readRemoteSdo(
        master, CANOPEN_SLAVE_NODE_ID, kTimeCobIdIndex,
        kTimeCobIdSubindex, read_back);
    if (read_result == SdoOperationResult::WAIT_TIMEOUT) {
        completion_wait_timed_out = true;
        return false;
    }
    if (read_result != SdoOperationResult::SUCCESS) {
        return false;
    }
    if (read_back != value) {
        spdlog::error(
            "TIME consumer validation 0x1012 read-back mismatch: expected=0x{:08x} "
            "actual=0x{:08x}",
            value, read_back);
        return false;
    }
    return true;
}

/**
 * @brief Establish a fresh slave communication/SDO boundary with Boot proof.
 */
bool resetSlaveCommunication(lely::canopen::AsyncMaster& master,
                             const char* description)
{
    prepareBootWait();
    if (!issueNmtCommand(master, lely::canopen::NmtCommand::RESET_COMM,
                         CANOPEN_SLAVE_NODE_ID, description)) {
        return false;
    }
    if (!waitForBootCompletion(
            std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS))) {
        spdlog::error("{} did not complete Boot", description);
        return false;
    }
    return true;
}

/**
 * @brief Verify 0x1012 without modifying it.
 */
bool verifyTimeCobId(lely::canopen::AsyncMaster& master,
                     std::uint32_t expected,
                     bool& completion_wait_timed_out)
{
    std::uint32_t actual = 0;
    const SdoOperationResult result = readRemoteSdo(
        master, CANOPEN_SLAVE_NODE_ID, kTimeCobIdIndex,
        kTimeCobIdSubindex, actual);
    if (result == SdoOperationResult::WAIT_TIMEOUT) {
        completion_wait_timed_out = true;
        return false;
    }
    if (result != SdoOperationResult::SUCCESS) {
        return false;
    }
    if (actual != expected) {
        spdlog::error(
            "TIME consumer validation restored 0x1012 mismatch: expected=0x{:08x} "
            "actual=0x{:08x}",
            expected, actual);
        return false;
    }
    return true;
}

/**
 * @brief Recover and strictly restore the original TIME communication state.
 *
 * A first reset aborts any unknown SDO transaction. The original 0x1012 is
 * then written and verified, followed by a second communication reset so the
 * runtime receive-buffer configuration is rebuilt from the restored value.
 */
bool recoverTimeCobIdWithReset(lely::canopen::AsyncMaster& master,
                               std::uint32_t original,
                               bool& completion_wait_timed_out,
                               bool& slave_needs_start)
{
    spdlog::warn(
        "TIME consumer validation recovering unverified TIME configuration with Reset Communication");

    if (!resetSlaveCommunication(
            master, "TIME consumer validation recovery boundary Reset Communication")) {
        slave_needs_start = true;
        return false;
    }
    slave_needs_start = true;
    completion_wait_timed_out = false;

    if (!writeAndVerifyTimeCobId(master, original,
                                 completion_wait_timed_out)) {
        return false;
    }

    /* Reinitialize CO_TIME from the restored OD value so an initially disabled
     * consumer does not retain the temporary receive-buffer registration. */
    if (!resetSlaveCommunication(
            master, "TIME consumer validation restore-state Reset Communication")) {
        return false;
    }
    slave_needs_start = true;
    completion_wait_timed_out = false;

    if (!verifyTimeCobId(master, original, completion_wait_timed_out)) {
        return false;
    }

    spdlog::info("TIME consumer validation original 0x1012 restored and runtime reinitialized");
    return true;
}

/**
 * @brief Verify one invalid-DLC TIME frame is not applied by the MCU.
 */
bool verifyInvalidDlc(lely::canopen::AsyncMaster& master,
                      std::uint32_t can_id, std::uint8_t dlc,
                      bool& completion_wait_timed_out)
{
    TimeDiagnostic before{};
    if (!readTimeDiagnostic(master, before, completion_wait_timed_out)) {
        return false;
    }

    if (!sendTimeFrame(master, can_id, dlc, 70000000U, 40000U)) {
        return false;
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(kNegativeObservationWaitMs));

    TimeDiagnostic after{};
    if (!readTimeDiagnostic(master, after, completion_wait_timed_out)) {
        return false;
    }
    if (!validateContinuousAdvance(before, after)) {
        spdlog::error("TIME consumer validation invalid DLC {} changed applied TIME state",
                      static_cast<unsigned int>(dlc));
        return false;
    }

    spdlog::info("TIME consumer validation invalid DLC {} ignored as expected",
                 static_cast<unsigned int>(dlc));
    return true;
}

} // namespace

int timeProcess(lely::canopen::AsyncMaster& master)
{
    int result = 0;
    bool original_cob_id_saved = false;
    bool cob_id_modification_attempted = false;
    bool restoration_verified = false;
    bool completion_wait_timed_out = false;
    bool slave_needs_start = false;
    /* Set when a communication-reset boundary itself fails; normal SDO cleanup
     * must not assume the remote SDO server is immediately usable afterward. */
    bool reset_recovery_required = false;
    bool consumer_enable_reset_performed = false;
    std::uint32_t original_cob_id = 0;

    /* Step 1: prove the MCU diagnostic contract exists before changing 0x1012.
     * This makes an older firmware fail without leaving any test side effect. */
    TimeDiagnostic baseline{};
    if (!readTimeDiagnostic(master, baseline, completion_wait_timed_out)) {
        spdlog::error(
            "TIME consumer validation diagnostic 0x2300:01..03 is unavailable or invalid");
        return 1;
    }

    /* Step 2: save and validate the target's original TIME communication role. */
    const SdoOperationResult original_read = readRemoteSdo(
        master, CANOPEN_SLAVE_NODE_ID, kTimeCobIdIndex,
        kTimeCobIdSubindex, original_cob_id);
    if (original_read != SdoOperationResult::SUCCESS) {
        return 1;
    }
    original_cob_id_saved = true;

    if ((original_cob_id & kTimeUnsupportedCobIdMask) != 0U) {
        spdlog::error(
            "TIME consumer validation unsupported TIME 0x1012 value for current CANopenNode target: "
            "0x{:08x}",
            original_cob_id);
        return 1;
    }
    if ((original_cob_id & kTimeProducerMask) != 0U) {
        spdlog::error(
            "TIME consumer validation requires the slave to remain a TIME consumer only; "
            "0x1012=0x{:08x} enables producer role",
            original_cob_id);
        return 1;
    }

    const std::uint32_t time_can_id = original_cob_id & kTimeCanIdMask;
    const std::uint32_t enabled_cob_id =
        original_cob_id | kTimeConsumerMask;

    /* Step 3: enable TIME consumption when the baseline has bit31 clear. The
     * current CANopenNode OD write hook updates isConsumer but does not install
     * a receive buffer when enabling from an initially disabled state, so a
     * communication reset is required after the write. */
    if (enabled_cob_id != original_cob_id) {
        cob_id_modification_attempted = true;
        if (!writeAndVerifyTimeCobId(master, enabled_cob_id,
                                     completion_wait_timed_out)) {
            result = 1;
        }
        if (result == 0) {
            if (!resetSlaveCommunication(
                    master, "TIME consumer validation enable-consumer Reset Communication")) {
                reset_recovery_required = true;
                result = 1;
            } else {
                consumer_enable_reset_performed = true;
                slave_needs_start = true;
                completion_wait_timed_out = false;
                if (!verifyTimeCobId(master, enabled_cob_id,
                                     completion_wait_timed_out)) {
                    result = 1;
                }
            }
        }
    }

    /* A communication reset may recreate the diagnostic state. Establish a
     * fresh baseline immediately before the actual TIME stimuli. */
    if (result == 0
        && !readTimeDiagnostic(master, baseline,
                               completion_wait_timed_out)) {
        result = 1;
    }

    /* Step 4: verify one ordinary valid TIME frame. */
    if (result == 0
        && !injectAndVerifyTime(master, time_can_id, 12345000U, 1000U,
                                completion_wait_timed_out)) {
        result = 1;
    }

    /* Step 5: millisecond lower/upper boundaries, including midnight rollover. */
    if (result == 0
        && !injectAndVerifyTime(master, time_can_id, 0U, 1001U,
                                completion_wait_timed_out)) {
        result = 1;
    }
    if (result == 0
        && !injectAndVerifyTime(master, time_can_id, 86399999U, 1002U,
                                completion_wait_timed_out)) {
        result = 1;
    }

    /* Step 6: day-count lower/upper boundaries. */
    if (result == 0
        && !injectAndVerifyTime(master, time_can_id, 43210000U, 0U,
                                completion_wait_timed_out)) {
        result = 1;
    }
    if (result == 0
        && !injectAndVerifyTime(master, time_can_id, 1000U, 0xFFFFU,
                                completion_wait_timed_out)) {
        result = 1;
    }

    /* Step 7: after one applied TIME, prove CO_TIME_process() continues to
     * advance the application time without receiving another timestamp. */
    if (result == 0) {
        TimeDiagnostic before{};
        if (!injectAndVerifyTime(master, time_can_id, 10000U, 2000U,
                                 completion_wait_timed_out, &before)) {
            result = 1;
        } else {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kContinuousWaitMs));
            TimeDiagnostic after{};
            if (!readTimeDiagnostic(master, after,
                                    completion_wait_timed_out)) {
                result = 1;
            } else {
                if (!validateContinuousAdvance(before, after)) {
                    result = 1;
                }
            }
        }
    }

    /* Step 8: CANopenNode accepts exactly six TIME bytes; both shorter and
     * longer Classic CAN payloads must leave the applied application state on
     * its normal internal progression path. */
    if (result == 0
        && !verifyInvalidDlc(master, time_can_id, 5U,
                             completion_wait_timed_out)) {
        result = 1;
    }
    if (result == 0
        && !verifyInvalidDlc(master, time_can_id, 7U,
                             completion_wait_timed_out)) {
        result = 1;
    }

    /* Step 9: disable the consumer dynamically and prove a syntactically valid
     * TIME frame reaches the already-configured receive callback but is not
     * applied by CO_TIME_process(). CANopenNode keeps the RX buffer installed
     * when bit31 is cleared and only gates application through isConsumer. */
    if (result == 0) {
        cob_id_modification_attempted = true;
        const std::uint32_t disabled_cob_id =
            enabled_cob_id & ~kTimeConsumerMask;
        if (!writeAndVerifyTimeCobId(master, disabled_cob_id,
                                     completion_wait_timed_out)) {
            result = 1;
        } else {
            TimeDiagnostic before{};
            if (!readTimeDiagnostic(master, before,
                                    completion_wait_timed_out)) {
                result = 1;
            } else {
                if (!sendTimeFrame(master, time_can_id, kTimeMessageLength,
                                   60000000U, 30000U)) {
                    result = 1;
                } else {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(kNegativeObservationWaitMs));
                    TimeDiagnostic after{};
                    if (!readTimeDiagnostic(master, after,
                                            completion_wait_timed_out)) {
                        result = 1;
                    } else {
                        if (!validateContinuousAdvance(before, after, 1U)) {
                            spdlog::error(
                                "TIME consumer validation disabled TIME consumer handling mismatch");
                            result = 1;
                        }
                    }
                }
            }
        }
    }

    /* Step 10: re-enable the consumer and require another valid TIME to apply. */
    if (result == 0) {
        if (!writeAndVerifyTimeCobId(master, enabled_cob_id,
                                     completion_wait_timed_out)) {
            result = 1;
        } else if (!injectAndVerifyTime(
                       master, time_can_id, 22345000U, 3000U,
                       completion_wait_timed_out)) {
            result = 1;
        }
    }

    /* Step 11: normal cleanup writes the exact original 0x1012. If TIME consumer validation had to
     * reset after enabling an initially disabled consumer, reset once more after
     * restoration so the runtime receive-buffer configuration also matches the
     * entry state, not only the OD value. */
    if (original_cob_id_saved && cob_id_modification_attempted
        && !completion_wait_timed_out && !reset_recovery_required) {
        bool normal_restore_ok =
            writeAndVerifyTimeCobId(master, original_cob_id,
                                    completion_wait_timed_out);
        if (normal_restore_ok && consumer_enable_reset_performed) {
            if (!resetSlaveCommunication(
                    master, "TIME consumer validation cleanup Reset Communication")) {
                normal_restore_ok = false;
            } else {
                slave_needs_start = true;
                completion_wait_timed_out = false;
                normal_restore_ok = verifyTimeCobId(
                    master, original_cob_id, completion_wait_timed_out);
            }
        }
        if (normal_restore_ok) {
            restoration_verified = true;
        } else {
            result = 1;
        }
    } else if (original_cob_id_saved && !cob_id_modification_attempted) {
        restoration_verified = true;
    }

    /* Step 12: any uncertain or unverified modified state gets a fresh reset,
     * explicit original-value write, second reset, and final read-back. */
    if (original_cob_id_saved && cob_id_modification_attempted
        && !restoration_verified) {
        result = 1;
        if (recoverTimeCobIdWithReset(master, original_cob_id,
                                      completion_wait_timed_out,
                                      slave_needs_start)) {
            restoration_verified = true;
        }
    }

    if (completion_wait_timed_out) {
        spdlog::error(
            "TIME consumer validation local SDO completion state remains unknown after cleanup");
        result = 1;
    }

    /* Any TIME consumer validation communication reset leaves the node in Boot-managed state. The
     * automatic sequence expects the slave Operational for later processes, so
     * restore that established post-PDO/SYNC validation process convention. */
    if (slave_needs_start && restoration_verified) {
        if (!issueNmtCommand(master, lely::canopen::NmtCommand::START,
                             CANOPEN_SLAVE_NODE_ID,
                             "TIME consumer validation cleanup slave NMT Start")) {
            result = 1;
        } else {
            slave_needs_start = false;
        }
    }

    if (cob_id_modification_attempted && !restoration_verified) {
        spdlog::error(
            "TIME consumer validation slave 0x1012 may remain changed because restoration was not "
            "verified");
        result = 1;
    }

    if (result == 0) {
        spdlog::info("TIME consumer validation passed");
    }
    return result;
}
