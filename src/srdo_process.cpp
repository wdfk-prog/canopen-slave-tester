/**
 * @file
 * @brief J09/B09S CANopenNode SRDO protocol validation implementation.
 */

#include "srdo_process.h"

#include "canopen_config.h"
#include "canopen_master.h"
#include "canopen_nmt.h"
#include "canopen_sdo.h"
#include "nmt_heartbeat.h"
#include "pdo_process.h"
#include "sync_pdo_process.h"

#include <lely/can/msg.h>
#include <lely/coapp/sdo_error.hpp>
#include <lely/io2/linux/can.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace {

constexpr std::uint16_t kSrdo1CommIndex = 0x1301U;
constexpr std::uint16_t kSrdo2CommIndex = 0x1302U;
constexpr std::uint16_t kSrdo1MapIndex = 0x1381U;
constexpr std::uint16_t kSrdo2MapIndex = 0x1382U;
constexpr std::uint16_t kSrdoConfigValidIndex = 0x13FEU;
constexpr std::uint16_t kSrdoChecksumIndex = 0x13FFU;
constexpr std::uint16_t kSrdoDiagnosticIndex = 0x2306U;

constexpr std::uint32_t kRxNormalCanId = 0x101U;
constexpr std::uint32_t kRxInvertedCanId = 0x102U;
constexpr std::uint32_t kTxNormalCanId = 0x103U;
constexpr std::uint32_t kTxInvertedCanId = 0x104U;
constexpr std::uint8_t kPayloadLength = 4U;
constexpr std::uint32_t kProbeNormal = 0x12345678UL;
constexpr std::uint32_t kProbeInverted = ~kProbeNormal;
constexpr std::uint32_t kWrongInverted = 0xAAAAAAAAUL;

constexpr std::uint16_t kProfileSctMs = 100U;
constexpr std::uint8_t kProfileSrvtMs = 20U;
constexpr std::uint8_t kProfileTransmissionType = 254U;
constexpr std::uint32_t kRxNormalMap = 0x23060120UL;
constexpr std::uint32_t kRxInvertedMap = 0x23060220UL;
constexpr std::uint32_t kTxNormalMap = 0x23060320UL;
constexpr std::uint32_t kTxInvertedMap = 0x23060420UL;

constexpr auto kRxPairDelay = std::chrono::milliseconds(5);
constexpr auto kDiagnosticPoll = std::chrono::milliseconds(10);
constexpr auto kF05StateSampleBudget = std::chrono::milliseconds(25);
constexpr auto kStateTimeout = std::chrono::milliseconds(1500);
constexpr auto kWireTimeout = std::chrono::milliseconds(1500);
constexpr auto kResetTimeout = std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS);
constexpr auto kSrvtFaultWait = std::chrono::milliseconds(30);
constexpr auto kSctFaultWait = std::chrono::milliseconds(130);
constexpr auto kNoTxObservation = std::chrono::milliseconds(150);
constexpr auto kRegressionRxPeriod = std::chrono::milliseconds(40);
constexpr auto kEstablishedHold = std::chrono::milliseconds(130);
constexpr auto kPairDeadline = std::chrono::milliseconds(50);
constexpr std::size_t kTxPairSamples = 200U;
constexpr std::size_t kResetNaturalPairSamples = 3U;
constexpr unsigned int kTxBusyMaxRetries = 4U;
constexpr std::int32_t kTxRequestResultIdle = std::numeric_limits<std::int32_t>::min();
/* Mirrored from pinned CANopenNode ef3de946... CO_ReturnError_t. Only this
 * transient result is retryable; every other result remains a DUT verdict. */
constexpr std::int32_t kTxRequestResultBusy = -15;
constexpr long long kCycleMinUs = 50000LL;
constexpr long long kCycleMaxUs = 150000LL;
constexpr long long kCycleAverageMinUs = 80000LL;
constexpr long long kCycleAverageMaxUs = 120000LL;
constexpr long long kPairMaxUs = 30000LL;

constexpr std::int8_t kStateErrorConfiguration = -9;
constexpr std::int8_t kStateTxNotInverted = -6;
constexpr std::int8_t kStateRxTimeoutSrvt = -4;
constexpr std::int8_t kStateRxTimeoutSct = -3;
constexpr std::int8_t kStateRxNotInverted = -2;
constexpr std::int8_t kStateRxShort = -1;
constexpr std::int8_t kStateNmtNotOperational = 1;
constexpr std::int8_t kStateEstablished = 3;

using SteadyClock = std::chrono::steady_clock;
using Deadline = SteadyClock::time_point;

struct SrdoDiagnostic {
    std::uint32_t rx_normal = 0U;
    std::uint32_t rx_inverted = 0U;
    std::uint32_t tx_normal = 0U;
    std::uint32_t tx_inverted = 0U;
    std::int8_t aggregate_state = 0;
    std::int8_t rx_state = 0;
    std::int8_t tx_state = 0;
    std::uint32_t state_seq = 0U;
    std::uint32_t tx_request_seq = 0U;
    std::uint32_t tx_complete_seq = 0U;
    std::int32_t tx_request_result = 0;
};

struct TxRequestOutcome {
    std::uint32_t sequence = 0U;
    std::int32_t result = kTxRequestResultIdle;
    unsigned int busy_retries = 0U;
};

struct SrdoCommProfile {
    std::uint8_t direction = 0U;
    std::uint16_t sct_ms = 0U;
    std::uint8_t srvt_ms = 0U;
    std::uint8_t transmission_type = 0U;
    std::uint32_t normal_id = 0U;
    std::uint32_t inverted_id = 0U;
};

struct SrdoMapProfile {
    std::uint8_t count = 0U;
    std::array<std::uint32_t, 2U> entry{{0U, 0U}};
};

struct SrdoProfile {
    std::uint8_t configuration_valid = 0U;
    std::uint8_t checksum_count = 0U;
    std::array<std::uint16_t, 2U> checksum{{0U, 0U}};
    std::array<SrdoCommProfile, 2U> comm;
    std::array<SrdoMapProfile, 2U> map;
};

struct WireFrame {
    can_msg message = CAN_MSG_INIT;
    std::chrono::nanoseconds timestamp{0};
};

enum class WireWaitResult { FRAME, TIMEOUT, ERROR };
enum class PairSyncState { EXPECT_NORMAL, EXPECT_INVERTED };

std::chrono::milliseconds remainingBudget(Deadline deadline) noexcept
{
    const auto now = SteadyClock::now();
    if (now >= deadline) {
        return std::chrono::milliseconds(0);
    }
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (remaining.count() == 0) {
        remaining = std::chrono::milliseconds(1);
    }
    return remaining;
}

class SrdoWireFixture {
public:
    explicit SrdoWireFixture(lely::io::CanChannel& channel) noexcept
        : channel_(channel)
    {
    }

    bool drain(std::chrono::milliseconds timeout = kWireTimeout) noexcept
    {
        const Deadline deadline = SteadyClock::now() + timeout;
        while (SteadyClock::now() < deadline) {
            can_msg message = CAN_MSG_INIT;
            std::chrono::nanoseconds timestamp{0};
            std::error_code error;
            const int result = channel_.read(&message, nullptr, &timestamp, 0, error);
            if (result < 0) {
                if (isWouldBlock(error)) {
                    return true;
                }
                spdlog::error("B09S wire drain failed: {}", error.message());
                return false;
            }
        }
        spdlog::error("B09S wire drain timed out after {} ms before channel became idle",
                      timeout.count());
        return false;
    }

    bool sendRx(std::uint32_t can_id, const std::uint8_t* data, std::uint8_t dlc) noexcept
    {
        if ((can_id != kRxNormalCanId && can_id != kRxInvertedCanId) || data == nullptr || dlc > 8U) {
            spdlog::error("B09S fixture rejected frame id=0x{:03x} dlc={}",
                          can_id, static_cast<unsigned int>(dlc));
            return false;
        }

        can_msg message = CAN_MSG_INIT;
        message.id = can_id;
        message.flags = 0U;
        message.len = dlc;
        std::memcpy(message.data, data, dlc);

        std::error_code error;
        channel_.write(message, 500, error);
        if (error) {
            spdlog::error("B09S wire send failed: id=0x{:03x}: {}", can_id, error.message());
            return false;
        }
        return true;
    }

    bool sendPair(std::uint32_t normal, std::uint32_t inverted,
                  std::chrono::milliseconds delay = kRxPairDelay) noexcept
    {
        const std::array<std::uint8_t, kPayloadLength> normal_bytes = encodeU32(normal);
        const std::array<std::uint8_t, kPayloadLength> inverted_bytes = encodeU32(inverted);
        if (!sendRx(kRxNormalCanId, normal_bytes.data(), kPayloadLength)) {
            return false;
        }
        std::this_thread::sleep_for(delay);
        return sendRx(kRxInvertedCanId, inverted_bytes.data(), kPayloadLength);
    }

    WireWaitResult waitForAnyTxUntil(Deadline deadline, WireFrame& frame) noexcept
    {
        while (SteadyClock::now() < deadline) {
            can_msg message = CAN_MSG_INIT;
            std::chrono::nanoseconds timestamp{0};
            std::error_code error;
            const int result = channel_.read(&message, nullptr, &timestamp,
                                             boundedTimeoutMs(remainingBudget(deadline)), error);
            if (result < 0) {
                if (isWouldBlock(error)) {
                    return WireWaitResult::TIMEOUT;
                }
                spdlog::error("B09S wire capture failed: {}", error.message());
                return WireWaitResult::ERROR;
            }
            if (result == 0) {
                continue;
            }
            if (message.id == kTxNormalCanId || message.id == kTxInvertedCanId) {
                frame.message = message;
                frame.timestamp = timestamp;
                return WireWaitResult::FRAME;
            }
        }
        return WireWaitResult::TIMEOUT;
    }

    WireWaitResult waitForAnyTx(std::chrono::milliseconds timeout, WireFrame& frame) noexcept
    {
        return waitForAnyTxUntil(SteadyClock::now() + timeout, frame);
    }

private:
    static bool isWouldBlock(const std::error_code& error) noexcept
    {
        return error.value() == EAGAIN || error.value() == EWOULDBLOCK;
    }

    static int boundedTimeoutMs(std::chrono::milliseconds timeout) noexcept
    {
        const long long value = timeout.count();
        if (value <= 0) {
            return 1;
        }
        if (value > static_cast<long long>(std::numeric_limits<int>::max())) {
            return std::numeric_limits<int>::max();
        }
        return static_cast<int>(value);
    }

    static std::array<std::uint8_t, kPayloadLength> encodeU32(std::uint32_t value) noexcept
    {
        return {{static_cast<std::uint8_t>(value & 0xFFU),
                 static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
                 static_cast<std::uint8_t>((value >> 16U) & 0xFFU),
                 static_cast<std::uint8_t>((value >> 24U) & 0xFFU)}};
    }

    lely::io::CanChannel& channel_;
};

class TxPairSynchronizer {
public:
    TxPairSynchronizer(SrdoWireFixture& fixture, bool allow_initial_resync) noexcept
        : fixture_(fixture), allow_initial_resync_(allow_initial_resync)
    {
    }

    bool nextPair(std::chrono::milliseconds timeout, WireFrame& normal, WireFrame& inverted,
                  const char* label, bool keep_rx_alive = false) noexcept
    {
        const Deadline overall_deadline = SteadyClock::now() + timeout;
        Deadline next_rx_keepalive = SteadyClock::now();
        while (SteadyClock::now() < overall_deadline) {
            if (keep_rx_alive && SteadyClock::now() >= next_rx_keepalive) {
                if (!fixture_.sendPair(kProbeNormal, kProbeInverted)) {
                    spdlog::error("{} RX keepalive failed during TX pair capture", label);
                    return false;
                }
                next_rx_keepalive = SteadyClock::now() + kRegressionRxPeriod;
            }

            WireFrame frame;
            Deadline read_deadline = overall_deadline;
            if (state_ == PairSyncState::EXPECT_INVERTED) {
                read_deadline = std::min(overall_deadline, normal_deadline_);
            }
            if (keep_rx_alive) {
                read_deadline = std::min(read_deadline, next_rx_keepalive);
            }

            const WireWaitResult wait = fixture_.waitForAnyTxUntil(read_deadline, frame);
            if (wait == WireWaitResult::ERROR) {
                return false;
            }
            if (wait == WireWaitResult::TIMEOUT) {
                const bool keepalive_deadline_reached = keep_rx_alive
                    && read_deadline == next_rx_keepalive
                    && next_rx_keepalive < overall_deadline
                    && (state_ != PairSyncState::EXPECT_INVERTED
                        || next_rx_keepalive < normal_deadline_);
                if (keepalive_deadline_reached) {
                    /* The channel timeout is rounded to milliseconds, so service
                     * the selected keepalive deadline directly instead of
                     * requiring steady_clock::now() to have crossed it exactly. */
                    if (!fixture_.sendPair(kProbeNormal, kProbeInverted)) {
                        spdlog::error("{} RX keepalive failed during TX pair capture", label);
                        return false;
                    }
                    next_rx_keepalive = SteadyClock::now() + kRegressionRxPeriod;
                    continue;
                }
                if (state_ == PairSyncState::EXPECT_INVERTED) {
                    spdlog::error("{} TX pair deadline expired waiting for inverted frame", label);
                } else {
                    spdlog::error("{} TX pair capture timed out waiting for normal frame", label);
                }
                return false;
            }

            if (state_ == PairSyncState::EXPECT_NORMAL) {
                if (frame.message.id == kTxInvertedCanId) {
                    if (allow_initial_resync_ && !synchronized_ && !initial_resync_used_) {
                        initial_resync_used_ = true;
                        spdlog::debug("{} discarded one leading inverted frame while synchronizing", label);
                        continue;
                    }
                    spdlog::error("{} TX ordering fault: inverted frame arrived before normal", label);
                    return false;
                }
                normal_ = frame;
                normal_deadline_ = SteadyClock::now() + kPairDeadline;
                state_ = PairSyncState::EXPECT_INVERTED;
                continue;
            }

            if (frame.message.id == kTxNormalCanId) {
                spdlog::error("{} TX ordering fault: duplicate normal before inverted", label);
                return false;
            }

            normal = normal_;
            inverted = frame;
            state_ = PairSyncState::EXPECT_NORMAL;
            synchronized_ = true;
            return true;
        }
        return false;
    }

private:
    SrdoWireFixture& fixture_;
    bool allow_initial_resync_ = false;
    bool synchronized_ = false;
    bool initial_resync_used_ = false;
    PairSyncState state_ = PairSyncState::EXPECT_NORMAL;
    WireFrame normal_;
    Deadline normal_deadline_{};
};

class ScopedRxPairWorker {
public:
    explicit ScopedRxPairWorker(SrdoWireFixture& fixture,
                                std::chrono::milliseconds period = kRegressionRxPeriod)
        : fixture_(fixture), period_(period), worker_(&ScopedRxPairWorker::run, this)
    {
    }

    ~ScopedRxPairWorker() noexcept
    {
        stop();
    }

    ScopedRxPairWorker(const ScopedRxPairWorker&) = delete;
    ScopedRxPairWorker& operator=(const ScopedRxPairWorker&) = delete;

    void stop() noexcept
    {
        running_.store(false, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    bool ok() const noexcept
    {
        return ok_.load(std::memory_order_acquire);
    }

private:
    void run() noexcept
    {
        while (running_.load(std::memory_order_acquire)) {
            if (!fixture_.sendPair(kProbeNormal, kProbeInverted)) {
                ok_.store(false, std::memory_order_release);
                return;
            }
            std::this_thread::sleep_for(period_);
        }
    }

    SrdoWireFixture& fixture_;
    std::chrono::milliseconds period_;
    std::atomic<bool> running_{true};
    std::atomic<bool> ok_{true};
    std::thread worker_;
};

class ScopedTxSilenceObserver {
public:
    explicit ScopedTxSilenceObserver(SrdoWireFixture& fixture)
        : fixture_(fixture), worker_(&ScopedTxSilenceObserver::run, this)
    {
    }

    ~ScopedTxSilenceObserver() noexcept
    {
        stop();
    }

    ScopedTxSilenceObserver(const ScopedTxSilenceObserver&) = delete;
    ScopedTxSilenceObserver& operator=(const ScopedTxSilenceObserver&) = delete;

    void stop() noexcept
    {
        running_.store(false, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    bool ok() const noexcept
    {
        return ok_.load(std::memory_order_acquire);
    }

    std::uint32_t unexpectedId() const noexcept
    {
        return unexpected_id_.load(std::memory_order_acquire);
    }

private:
    void run() noexcept
    {
        while (running_.load(std::memory_order_acquire)) {
            WireFrame frame;
            const WireWaitResult wait = fixture_.waitForAnyTx(kDiagnosticPoll, frame);
            if (wait == WireWaitResult::ERROR) {
                ok_.store(false, std::memory_order_release);
                return;
            }
            if (wait == WireWaitResult::FRAME) {
                unexpected_id_.store(frame.message.id, std::memory_order_release);
                return;
            }
        }
    }

    SrdoWireFixture& fixture_;
    std::atomic<bool> running_{true};
    std::atomic<bool> ok_{true};
    std::atomic<std::uint32_t> unexpected_id_{0U};
    std::thread worker_;
};

template <class T>
bool readRemoteUntil(lely::canopen::AsyncMaster& master, std::uint16_t index,
                     std::uint8_t subindex, T& value, const char* label, Deadline deadline)
{
    struct State {
        std::mutex mutex;
        std::condition_variable condition;
        bool completed = false;
        std::error_code error;
        T value{};
    };

    const auto budget = remainingBudget(deadline);
    if (budget.count() <= 0) {
        spdlog::error("B09S remote read budget exhausted: {}", label);
        return false;
    }

    const auto state = std::make_shared<State>();
    std::error_code submit_error;
    master.SubmitRead<T>(
        master.GetExecutor(), CANOPEN_SLAVE_NODE_ID, index, subindex,
        [state](std::uint8_t, std::uint16_t, std::uint8_t, std::error_code error, T read_value) noexcept {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->completed = true;
                state->error = error;
                state->value = read_value;
            }
            state->condition.notify_all();
        },
        budget, submit_error);

    if (submit_error) {
        spdlog::error("B09S remote read submission failed: {}: {}", label, submit_error.message());
        return false;
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_until(lock, deadline, [state]() { return state->completed; })) {
        spdlog::error("B09S remote read timed out within total budget: {}", label);
        return false;
    }
    if (state->error) {
        spdlog::error("B09S remote read failed: {}: {}", label, state->error.message());
        return false;
    }
    value = state->value;
    return true;
}

template <class T>
bool readRemote(lely::canopen::AsyncMaster& master, std::uint16_t index,
                std::uint8_t subindex, T& value, const char* label)
{
    return readRemoteUntil(master, index, subindex, value, label, SteadyClock::now() + kStateTimeout);
}

template <class T>
bool writeRemote(lely::canopen::AsyncMaster& master, std::uint16_t index,
                 std::uint8_t subindex, T value, const char* label)
{
    if (writeRemoteSdo<T>(master, CANOPEN_SLAVE_NODE_ID, index, subindex, value,
                          std::chrono::milliseconds(500), std::chrono::milliseconds(100))
        != SdoOperationResult::SUCCESS) {
        spdlog::error("B09S remote write failed: {}", label);
        return false;
    }
    return true;
}

template <class T>
bool requireWriteRejected(lely::canopen::AsyncMaster& master, std::uint16_t index,
                          std::uint8_t subindex, T value,
                          lely::canopen::SdoErrc expected_abort, const char* label)
{
    struct State {
        std::mutex mutex;
        std::condition_variable condition;
        bool completed = false;
        std::error_code error;
    };

    const auto state = std::make_shared<State>();
    std::error_code submit_error;
    master.SubmitWrite(
        master.GetExecutor(), CANOPEN_SLAVE_NODE_ID, index, subindex, value,
        [state](std::uint8_t, std::uint16_t, std::uint8_t, std::error_code error) noexcept {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->completed = true;
                state->error = error;
            }
            state->condition.notify_all();
        },
        std::chrono::milliseconds(500), submit_error);

    if (submit_error) {
        spdlog::error("B09S rejection probe submission failed: {}: {}", label, submit_error.message());
        return false;
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(lock, std::chrono::milliseconds(600),
                                   [state]() { return state->completed; })) {
        spdlog::error("B09S rejection probe completion timed out: {}", label);
        return false;
    }
    if (!state->error) {
        spdlog::error("B09S invalid write was unexpectedly accepted: {}", label);
        return false;
    }
    if (state->error.category() != lely::canopen::SdoCategory()) {
        spdlog::error("B09S rejection probe failed locally instead of receiving SDO abort: {}: {}",
                      label, state->error.message());
        return false;
    }

    const lely::canopen::SdoErrc abort = lely::canopen::sdo_errc(state->error);
    if (abort == lely::canopen::SdoErrc::TIMEOUT) {
        spdlog::error("B09S invalid write timed out instead of being rejected: {}", label);
        return false;
    }
    if (abort != expected_abort) {
        spdlog::error(
            "B09S invalid write returned unexpected SDO abort: {} expected=0x{:08x} actual=0x{:08x}",
            label, static_cast<std::uint32_t>(expected_abort), static_cast<std::uint32_t>(abort));
        return false;
    }
    spdlog::info("B09S expected SDO rejection: {} abort=0x{:08x}", label,
                 static_cast<std::uint32_t>(abort));
    return true;
}

bool readDiagnosticUntil(lely::canopen::AsyncMaster& master, SrdoDiagnostic& value, Deadline deadline)
{
    for (unsigned int attempt = 0U; attempt < 4U && SteadyClock::now() < deadline; ++attempt) {
        std::uint32_t begin = 0U;
        std::uint32_t end = 0U;
        if (!readRemoteUntil(master, kSrdoDiagnosticIndex, 0x08U, begin, "0x2306:08 state_seq begin", deadline)
            || !readRemoteUntil(master, kSrdoDiagnosticIndex, 0x01U, value.rx_normal, "0x2306:01 rx_normal", deadline)
            || !readRemoteUntil(master, kSrdoDiagnosticIndex, 0x02U, value.rx_inverted, "0x2306:02 rx_inverted", deadline)
            || !readRemoteUntil(master, kSrdoDiagnosticIndex, 0x03U, value.tx_normal, "0x2306:03 tx_normal", deadline)
            || !readRemoteUntil(master, kSrdoDiagnosticIndex, 0x04U, value.tx_inverted, "0x2306:04 tx_inverted", deadline)
            || !readRemoteUntil(master, kSrdoDiagnosticIndex, 0x05U, value.aggregate_state,
                                "0x2306:05 aggregate_state", deadline)
            || !readRemoteUntil(master, kSrdoDiagnosticIndex, 0x06U, value.rx_state, "0x2306:06 rx_state", deadline)
            || !readRemoteUntil(master, kSrdoDiagnosticIndex, 0x07U, value.tx_state, "0x2306:07 tx_state", deadline)
            || !readRemoteUntil(master, kSrdoDiagnosticIndex, 0x09U, value.tx_request_seq,
                                "0x2306:09 tx_request_seq", deadline)
            || !readRemoteUntil(master, kSrdoDiagnosticIndex, 0x0AU, value.tx_complete_seq,
                                "0x2306:0A tx_complete_seq", deadline)
            || !readRemoteUntil(master, kSrdoDiagnosticIndex, 0x0BU, value.tx_request_result,
                                "0x2306:0B tx_request_result", deadline)
            || !readRemoteUntil(master, kSrdoDiagnosticIndex, 0x08U, end, "0x2306:08 state_seq end", deadline)) {
            return false;
        }
        if (begin == end) {
            value.state_seq = end;
            return true;
        }
    }
    spdlog::error("B09S diagnostic snapshot remained unstable or exhausted its total budget");
    return false;
}

bool readDiagnostic(lely::canopen::AsyncMaster& master, SrdoDiagnostic& value)
{
    return readDiagnosticUntil(master, value, SteadyClock::now() + kStateTimeout);
}

bool waitForDiagnosticState(lely::canopen::AsyncMaster& master, std::int8_t expected_rx,
                            std::int8_t expected_tx, std::int8_t expected_aggregate,
                            std::chrono::milliseconds timeout = kStateTimeout)
{
    const Deadline deadline = SteadyClock::now() + timeout;
    while (SteadyClock::now() < deadline) {
        std::int8_t rx_state = 0;
        std::int8_t tx_state = 0;
        std::int8_t aggregate_state = 0;
        if (!readRemoteUntil(master, kSrdoDiagnosticIndex, 0x06U, rx_state,
                             "0x2306:06 rx_state", deadline)
            || !readRemoteUntil(master, kSrdoDiagnosticIndex, 0x07U, tx_state,
                                "0x2306:07 tx_state", deadline)
            || !readRemoteUntil(master, kSrdoDiagnosticIndex, 0x05U, aggregate_state,
                                "0x2306:05 aggregate_state", deadline)) {
            return false;
        }
        if (rx_state == expected_rx && tx_state == expected_tx
            && aggregate_state == expected_aggregate) {
            return true;
        }
        std::this_thread::sleep_for(std::min(kDiagnosticPoll, remainingBudget(deadline)));
    }
    spdlog::error("B09S state wait timed out: expected rx/tx/agg={}/{}/{}",
                  static_cast<int>(expected_rx), static_cast<int>(expected_tx),
                  static_cast<int>(expected_aggregate));
    return false;
}

bool waitForRxState(lely::canopen::AsyncMaster& master, std::int8_t expected,
                    std::chrono::milliseconds timeout = kStateTimeout)
{
    const Deadline deadline = SteadyClock::now() + timeout;
    while (SteadyClock::now() < deadline) {
        std::int8_t rx_state = 0;
        if (!readRemoteUntil(master, kSrdoDiagnosticIndex, 0x06U, rx_state,
                             "0x2306:06 rx_state", deadline)) {
            return false;
        }
        if (rx_state == expected) {
            return true;
        }
        if (rx_state < 0 && rx_state != expected) {
            spdlog::error("B09S unexpected RX state: expected={} actual={}",
                          static_cast<int>(expected), static_cast<int>(rx_state));
            return false;
        }
        std::this_thread::sleep_for(std::min(kDiagnosticPoll, remainingBudget(deadline)));
    }
    return false;
}

bool waitForTxState(lely::canopen::AsyncMaster& master, std::int8_t expected,
                    std::chrono::milliseconds timeout = kStateTimeout)
{
    const Deadline deadline = SteadyClock::now() + timeout;
    while (SteadyClock::now() < deadline) {
        std::int8_t tx_state = 0;
        if (!readRemoteUntil(master, kSrdoDiagnosticIndex, 0x07U, tx_state,
                             "0x2306:07 tx_state", deadline)) {
            return false;
        }
        if (tx_state == expected) {
            return true;
        }
        if (tx_state < 0 && tx_state != expected) {
            spdlog::error("B09S unexpected TX state: expected={} actual={}",
                          static_cast<int>(expected), static_cast<int>(tx_state));
            return false;
        }
        std::this_thread::sleep_for(std::min(kDiagnosticPoll, remainingBudget(deadline)));
    }
    return false;
}

bool enterPreOperational(CanopenTestMaster& master)
{
    return issueNmtCommandAndWaitForState(master, lely::canopen::NmtCommand::ENTER_PREOP,
                                          CANOPEN_SLAVE_NODE_ID, lely::canopen::NmtState::PREOP,
                                          kResetTimeout, "B09S enter Pre-operational");
}

bool enterOperational(CanopenTestMaster& master)
{
    return issueNmtCommandAndWaitForState(master, lely::canopen::NmtCommand::START,
                                          CANOPEN_SLAVE_NODE_ID, lely::canopen::NmtState::START,
                                          kResetTimeout, "B09S enter Operational");
}

bool resetCommunication(CanopenTestMaster& master)
{
    prepareBootWait();
    if (!issueNmtCommand(master, lely::canopen::NmtCommand::RESET_COMM,
                         CANOPEN_SLAVE_NODE_ID, "B09S Reset Communication")) {
        return false;
    }

    lely::canopen::NmtState state = lely::canopen::NmtState::BOOTUP;
    if (!waitForBootCompletion(kResetTimeout, state)) {
        spdlog::error("B09S Boot after Reset Communication timed out");
        return false;
    }
    return state == lely::canopen::NmtState::START || enterOperational(master);
}

std::uint16_t crc16CcittByte(std::uint16_t crc, std::uint8_t byte) noexcept
{
    crc ^= static_cast<std::uint16_t>(byte) << 8U;
    for (unsigned int bit = 0U; bit < 8U; ++bit) {
        crc = (crc & 0x8000U) != 0U
                  ? static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U)
                  : static_cast<std::uint16_t>(crc << 1U);
    }
    return crc;
}

void crcAppendU16Le(std::uint16_t& crc, std::uint16_t value) noexcept
{
    crc = crc16CcittByte(crc, static_cast<std::uint8_t>(value & 0xFFU));
    crc = crc16CcittByte(crc, static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void crcAppendU32Le(std::uint16_t& crc, std::uint32_t value) noexcept
{
    for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
        crc = crc16CcittByte(crc, static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

std::uint16_t calculateSrdoChecksum(const SrdoCommProfile& comm, const SrdoMapProfile& map) noexcept
{
    std::uint16_t crc = 0U;
    crc = crc16CcittByte(crc, comm.direction);
    crcAppendU16Le(crc, comm.sct_ms);
    crc = crc16CcittByte(crc, comm.srvt_ms);
    crcAppendU32Le(crc, comm.normal_id);
    crcAppendU32Le(crc, comm.inverted_id);
    crc = crc16CcittByte(crc, map.count);
    for (std::uint8_t i = 0U; i < map.count && i < map.entry.size(); ++i) {
        crc = crc16CcittByte(crc, static_cast<std::uint8_t>(i + 1U));
        crcAppendU32Le(crc, map.entry[i]);
    }
    return crc;
}

bool readComm(lely::canopen::AsyncMaster& master, std::uint16_t index, SrdoCommProfile& comm)
{
    return readRemote(master, index, 0x01U, comm.direction, "SRDO direction")
           && readRemote(master, index, 0x02U, comm.sct_ms, "SRDO SCT")
           && readRemote(master, index, 0x03U, comm.srvt_ms, "SRDO SRVT")
           && readRemote(master, index, 0x04U, comm.transmission_type, "SRDO transmission type")
           && readRemote(master, index, 0x05U, comm.normal_id, "SRDO normal CAN-ID")
           && readRemote(master, index, 0x06U, comm.inverted_id, "SRDO inverted CAN-ID");
}

bool readMap(lely::canopen::AsyncMaster& master, std::uint16_t index, SrdoMapProfile& map)
{
    if (!readRemote(master, index, 0x00U, map.count, "SRDO mapping count") || map.count != 2U) {
        spdlog::error("B09S test profile requires exactly two mapping entries: index=0x{:04x} count={}",
                      index, static_cast<unsigned int>(map.count));
        return false;
    }
    return readRemote(master, index, 0x01U, map.entry[0], "SRDO mapping 1")
           && readRemote(master, index, 0x02U, map.entry[1], "SRDO mapping 2");
}

bool readProfile(lely::canopen::AsyncMaster& master, SrdoProfile& profile)
{
    return readRemote(master, kSrdoConfigValidIndex, 0x00U, profile.configuration_valid, "0x13FE:00")
           && readRemote(master, kSrdoChecksumIndex, 0x00U, profile.checksum_count, "0x13FF:00")
           && readRemote(master, kSrdoChecksumIndex, 0x01U, profile.checksum[0], "0x13FF:01")
           && readRemote(master, kSrdoChecksumIndex, 0x02U, profile.checksum[1], "0x13FF:02")
           && readComm(master, kSrdo1CommIndex, profile.comm[0])
           && readComm(master, kSrdo2CommIndex, profile.comm[1])
           && readMap(master, kSrdo1MapIndex, profile.map[0])
           && readMap(master, kSrdo2MapIndex, profile.map[1]);
}

bool validateProfile(lely::canopen::AsyncMaster& master, SrdoProfile& profile)
{
    if (!readProfile(master, profile)) {
        return false;
    }

    const std::uint16_t calculated_rx = calculateSrdoChecksum(profile.comm[0], profile.map[0]);
    const std::uint16_t calculated_tx = calculateSrdoChecksum(profile.comm[1], profile.map[1]);
    const bool exact_profile = profile.configuration_valid == 0xA5U && profile.checksum_count >= 2U
        && profile.comm[0].direction == 2U && profile.comm[1].direction == 1U
        && profile.comm[0].sct_ms == kProfileSctMs && profile.comm[1].sct_ms == kProfileSctMs
        && profile.comm[0].srvt_ms == kProfileSrvtMs && profile.comm[1].srvt_ms == kProfileSrvtMs
        && profile.comm[0].transmission_type == kProfileTransmissionType
        && profile.comm[1].transmission_type == kProfileTransmissionType
        && profile.comm[0].normal_id == kRxNormalCanId && profile.comm[0].inverted_id == kRxInvertedCanId
        && profile.comm[1].normal_id == kTxNormalCanId && profile.comm[1].inverted_id == kTxInvertedCanId
        && profile.map[0].entry[0] == kRxNormalMap && profile.map[0].entry[1] == kRxInvertedMap
        && profile.map[1].entry[0] == kTxNormalMap && profile.map[1].entry[1] == kTxInvertedMap
        && profile.checksum[0] == calculated_rx && profile.checksum[1] == calculated_tx;

    if (!exact_profile) {
        spdlog::error(
            "B09S profile mismatch: valid=0x{:02x} crc=0x{:04x}/0x{:04x} "
            "calculated=0x{:04x}/0x{:04x} dir={}/{} SCT={}/{} SRVT={}/{} "
            "ids={:03x}/{:03x}/{:03x}/{:03x}",
            static_cast<unsigned int>(profile.configuration_valid), profile.checksum[0], profile.checksum[1],
            calculated_rx, calculated_tx, static_cast<unsigned int>(profile.comm[0].direction),
            static_cast<unsigned int>(profile.comm[1].direction), profile.comm[0].sct_ms, profile.comm[1].sct_ms,
            static_cast<unsigned int>(profile.comm[0].srvt_ms), static_cast<unsigned int>(profile.comm[1].srvt_ms),
            profile.comm[0].normal_id, profile.comm[0].inverted_id,
            profile.comm[1].normal_id, profile.comm[1].inverted_id);
        return false;
    }
    spdlog::info("B09S profile passed: RX checksum=0x{:04x} TX checksum=0x{:04x}",
                 profile.checksum[0], profile.checksum[1]);
    return true;
}

bool writeSrdo1Baseline(lely::canopen::AsyncMaster& master, const SrdoProfile& baseline)
{
    const SrdoCommProfile& comm = baseline.comm[0];
    const SrdoMapProfile& map = baseline.map[0];
    return writeRemote(master, kSrdo1CommIndex, 0x01U, static_cast<std::uint8_t>(0U), "disable SRDO1")
        && writeRemote(master, kSrdo1MapIndex, 0x00U, static_cast<std::uint8_t>(0U), "clear SRDO1 map count")
        && writeRemote(master, kSrdo1MapIndex, 0x01U, map.entry[0], "restore SRDO1 map 1")
        && writeRemote(master, kSrdo1MapIndex, 0x02U, map.entry[1], "restore SRDO1 map 2")
        && writeRemote(master, kSrdo1MapIndex, 0x00U, map.count, "restore SRDO1 map count")
        && writeRemote(master, kSrdo1CommIndex, 0x02U, comm.sct_ms, "restore SRDO1 SCT")
        && writeRemote(master, kSrdo1CommIndex, 0x03U, comm.srvt_ms, "restore SRDO1 SRVT")
        && writeRemote(master, kSrdo1CommIndex, 0x04U, comm.transmission_type, "restore SRDO1 type")
        && writeRemote(master, kSrdo1CommIndex, 0x05U, comm.normal_id, "restore SRDO1 normal CAN-ID")
        && writeRemote(master, kSrdo1CommIndex, 0x06U, comm.inverted_id, "restore SRDO1 inverted CAN-ID")
        && writeRemote(master, kSrdo1CommIndex, 0x01U, comm.direction, "restore SRDO1 direction")
        && writeRemote(master, kSrdoChecksumIndex, 0x01U, baseline.checksum[0], "restore SRDO1 checksum")
        && writeRemote(master, kSrdoConfigValidIndex, 0x00U, static_cast<std::uint8_t>(0xA5U),
                       "restore SRDO configuration-valid");
}

bool prepareCleanOperational(CanopenTestMaster& master, const SrdoProfile& baseline)
{
    return enterPreOperational(master) && writeSrdo1Baseline(master, baseline) && resetCommunication(master);
}

std::uint32_t decodeU32(const can_msg& message) noexcept
{
    return static_cast<std::uint32_t>(message.data[0])
           | (static_cast<std::uint32_t>(message.data[1]) << 8U)
           | (static_cast<std::uint32_t>(message.data[2]) << 16U)
           | (static_cast<std::uint32_t>(message.data[3]) << 24U);
}

bool validateWirePair(const WireFrame& normal, const WireFrame& inverted,
                      std::uint32_t expected_normal, const char* label)
{
    if (normal.message.id != kTxNormalCanId || inverted.message.id != kTxInvertedCanId
        || normal.message.flags != 0U || inverted.message.flags != 0U
        || normal.message.len != kPayloadLength || inverted.message.len != kPayloadLength) {
        spdlog::error("{} wire-format mismatch", label);
        return false;
    }
    for (std::uint8_t i = 0U; i < kPayloadLength; ++i) {
        if (inverted.message.data[i] != static_cast<std::uint8_t>(~normal.message.data[i])) {
            spdlog::error("{} inverse mismatch at byte {}", label, static_cast<unsigned int>(i));
            return false;
        }
    }
    if (decodeU32(normal.message) != expected_normal) {
        spdlog::error("{} normal payload mismatch: expected=0x{:08x} actual=0x{:08x}",
                      label, expected_normal, decodeU32(normal.message));
        return false;
    }
    if (inverted.timestamp < normal.timestamp) {
        spdlog::error("{} timestamp ordering invalid", label);
        return false;
    }
    return true;
}

bool waitForTxCompletion(lely::canopen::AsyncMaster& master, std::uint32_t request_seq,
                         std::int32_t& result)
{
    const Deadline deadline = SteadyClock::now() + kStateTimeout;
    while (SteadyClock::now() < deadline) {
        std::uint32_t complete_seq = 0U;
        if (!readRemoteUntil(master, kSrdoDiagnosticIndex, 0x0AU, complete_seq,
                             "0x2306:0A tx_complete_seq", deadline)) {
            return false;
        }
        if (complete_seq == request_seq) {
            return readRemoteUntil(master, kSrdoDiagnosticIndex, 0x0BU, result,
                                   "0x2306:0B tx_request_result", deadline);
        }
        std::this_thread::sleep_for(std::min(kDiagnosticPoll, remainingBudget(deadline)));
    }
    spdlog::error("B09S TX request timed out: seq={}", request_seq);
    return false;
}

bool synchronizeTxPhaseAfterBusy(SrdoWireFixture& fixture, const char* label)
{
    bool normal_seen = false;

    /* TX_BUSY means CANopenNode already emitted a normal frame and is waiting to
     * emit its inverted mate. Refresh RX first so phase recovery itself cannot
     * consume the 100 ms RX SCT budget, then consume the pending TX pair through
     * its inverted member. The next diagnostic request is therefore issued only
     * after nextIsNormal has returned to the requestable phase. */
    if (!fixture.sendPair(kProbeNormal, kProbeInverted)) {
        spdlog::error("{} TX_BUSY phase synchronization RX keepalive failed", label);
        return false;
    }

    const Deadline deadline = SteadyClock::now() + kPairDeadline;
    while (SteadyClock::now() < deadline) {
        WireFrame frame;
        const WireWaitResult wait = fixture.waitForAnyTxUntil(deadline, frame);
        if (wait == WireWaitResult::ERROR) {
            return false;
        }
        if (wait == WireWaitResult::TIMEOUT) {
            spdlog::error("{} TX_BUSY phase synchronization timed out waiting for pending inverted frame", label);
            return false;
        }

        if (frame.message.id == kTxNormalCanId) {
            if (normal_seen) {
                spdlog::error("{} TX_BUSY phase synchronization saw duplicate normal before inverted", label);
                return false;
            }
            normal_seen = true;
            continue;
        }

        spdlog::debug("{} TX_BUSY phase synchronized on pending inverted frame: normal_seen={}",
                      label, normal_seen);
        return true;
    }

    spdlog::error("{} TX_BUSY phase synchronization exhausted its deadline", label);
    return false;
}

bool requestTxWithBusyRetry(lely::canopen::AsyncMaster& master, SrdoWireFixture& fixture,
                            std::uint32_t completed_seq, TxRequestOutcome& outcome, const char* label)
{
    outcome.sequence = completed_seq;
    outcome.result = kTxRequestResultIdle;
    outcome.busy_retries = 0U;

    for (unsigned int attempt = 0U; attempt <= kTxBusyMaxRetries; ++attempt) {
        outcome.sequence++;
        if (!writeRemote(master, kSrdoDiagnosticIndex, 0x09U, outcome.sequence,
                         "0x2306:09 tx_request_seq")
            || !waitForTxCompletion(master, outcome.sequence, outcome.result)) {
            return false;
        }

        if (outcome.result != kTxRequestResultBusy) {
            spdlog::info("{} TX request completed: seq={} busy_retries={} final_result={}",
                         label, outcome.sequence, outcome.busy_retries, outcome.result);
            return true;
        }

        if (attempt == kTxBusyMaxRetries) {
            spdlog::error("{} TX request remained busy after {} retries: seq={} final_result={}",
                          label, outcome.busy_retries, outcome.sequence, outcome.result);
            return true;
        }

        outcome.busy_retries++;
        spdlog::warn("{} TX request hit transient CO_ERROR_TX_BUSY: seq={} retry={}/{}; synchronizing phase",
                     label, outcome.sequence, outcome.busy_retries, kTxBusyMaxRetries);
        if (!synchronizeTxPhaseAfterBusy(fixture, label)) {
            return false;
        }
    }

    return false;
}

bool triggerTx(lely::canopen::AsyncMaster& master, SrdoWireFixture& fixture,
               std::uint32_t normal_value, std::uint32_t inverted_value,
               WireFrame* normal_frame, WireFrame* inverted_frame, std::int32_t& request_result)
{
    SrdoDiagnostic diagnostic;
    request_result = kTxRequestResultIdle;
    if (!readDiagnostic(master, diagnostic) || diagnostic.tx_request_seq != diagnostic.tx_complete_seq) {
        spdlog::error("B09S refuses TX trigger with a pending/unreadable request");
        return false;
    }
    if (!writeRemote(master, kSrdoDiagnosticIndex, 0x03U, normal_value, "0x2306:03 tx_normal")
        || !writeRemote(master, kSrdoDiagnosticIndex, 0x04U, inverted_value, "0x2306:04 tx_inverted")
        || !fixture.drain()) {
        return false;
    }

    TxRequestOutcome request;
    if (!requestTxWithBusyRetry(master, fixture, diagnostic.tx_request_seq, request, "B09S triggered TX")) {
        return false;
    }
    request_result = request.result;
    if (request_result != 0) {
        return true;
    }

    if (normal_frame != nullptr || inverted_frame != nullptr) {
        if (normal_frame == nullptr || inverted_frame == nullptr) {
            spdlog::error("B09S TX capture requires both pair frame outputs");
            return false;
        }
        TxPairSynchronizer synchronizer(fixture, false);
        if (!synchronizer.nextPair(kWireTimeout, *normal_frame, *inverted_frame, "B09S triggered TX")) {
            return false;
        }
    }
    return true;
}

bool validateNmtNonOperational(CanopenTestMaster& master)
{
    if (!enterPreOperational(master)
        || !waitForDiagnosticState(master, kStateNmtNotOperational, kStateNmtNotOperational,
                                   kStateNmtNotOperational)) {
        spdlog::error("B09S-02 NMT non-operational state mismatch");
        return false;
    }
    spdlog::info("B09S-02 NMT non-operational state passed");
    return true;
}

bool validateRxPair(lely::canopen::AsyncMaster& master, SrdoWireFixture& fixture)
{
    if (!fixture.sendPair(kProbeNormal, kProbeInverted)) {
        return false;
    }

    /* Mapping/state validation is control-plane work and must not consume the
     * 100 ms RX SCT budget. Keep valid RX traffic flowing while the Host reads
     * only the fields needed by B09S-03/04. */
    ScopedRxPairWorker keepalive(fixture);
    std::uint32_t rx_normal = 0U;
    std::uint32_t rx_inverted = 0U;
    if (!waitForRxState(master, kStateEstablished)
        || !readRemote(master, kSrdoDiagnosticIndex, 0x01U, rx_normal,
                       "0x2306:01 rx_normal")
        || !readRemote(master, kSrdoDiagnosticIndex, 0x02U, rx_inverted,
                       "0x2306:02 rx_inverted")
        || !keepalive.ok() || rx_normal != kProbeNormal || rx_inverted != kProbeInverted) {
        spdlog::error("B09S-04 RX mapping/state validation failed");
        return false;
    }
    spdlog::info("B09S-03 RX valid pair passed");
    spdlog::info("B09S-04 RX mapped OD passed");
    return true;
}

bool validateTxPair(lely::canopen::AsyncMaster& master, SrdoWireFixture& fixture)
{
    WireFrame normal;
    WireFrame inverted;
    std::int32_t request_result = 0;
    if (!triggerTx(master, fixture, kProbeNormal, kProbeInverted, &normal, &inverted, request_result)
        || request_result != 0 || !validateWirePair(normal, inverted, kProbeNormal, "B09S-05")) {
        spdlog::error("B09S-05 TX pair validation failed: result={}", request_result);
        return false;
    }
    const auto delta = std::chrono::duration_cast<std::chrono::microseconds>(inverted.timestamp - normal.timestamp);
    if (delta.count() > kPairMaxUs) {
        spdlog::error("B09S-05 pair delay outside SRVT tolerance: {} us limit={} us",
                      delta.count(), kPairMaxUs);
        return false;
    }
    spdlog::info("B09S-05 TX pair passed: pair_delta_us={}", delta.count());
    return waitForTxState(master, kStateEstablished);
}

bool validateEstablishedWindow(lely::canopen::AsyncMaster& master, SrdoWireFixture& fixture,
                               const char* label)
{
    ScopedRxPairWorker keepalive(fixture);
    if (!waitForDiagnosticState(master, kStateEstablished, kStateEstablished, kStateEstablished)) {
        return false;
    }
    const Deadline hold_deadline = SteadyClock::now() + kEstablishedHold;
    while (SteadyClock::now() < hold_deadline) {
        if (!keepalive.ok()) {
            spdlog::error("{} RX keepalive failed while covering SCT window", label);
            return false;
        }
        std::this_thread::sleep_for(std::min(std::chrono::milliseconds(10), remainingBudget(hold_deadline)));
    }

    SrdoDiagnostic diagnostic;
    if (!keepalive.ok() || !readDiagnostic(master, diagnostic)
        || diagnostic.rx_state != kStateEstablished || diagnostic.tx_state != kStateEstablished
        || diagnostic.aggregate_state != kStateEstablished
        || diagnostic.rx_normal != kProbeNormal || diagnostic.rx_inverted != kProbeInverted) {
        spdlog::error("{} established state did not remain valid across SCT window", label);
        return false;
    }
    spdlog::info("{} established state held across {} ms RX SCT observation", label, kEstablishedHold.count());
    return true;
}

bool validateTxCycles(lely::canopen::AsyncMaster& master, SrdoWireFixture& fixture)
{
    if (!fixture.drain()) {
        return false;
    }

    TxPairSynchronizer synchronizer(fixture, true);
    std::vector<long long> pair_delta_us;
    std::vector<long long> cycle_delta_us;
    pair_delta_us.reserve(kTxPairSamples);
    cycle_delta_us.reserve(kTxPairSamples - 1U);
    std::chrono::nanoseconds previous_normal{0};

    for (std::size_t i = 0U; i < kTxPairSamples; ++i) {
        WireFrame normal;
        WireFrame inverted;
        if (!synchronizer.nextPair(kWireTimeout, normal, inverted, "B09S-06")
            || !validateWirePair(normal, inverted, kProbeNormal, "B09S-06")) {
            spdlog::error("B09S-06 TX cycle pair {} failed", i);
            return false;
        }
        const long long pair_delta = std::chrono::duration_cast<std::chrono::microseconds>(
                                         inverted.timestamp - normal.timestamp).count();
        if (pair_delta < 0 || pair_delta > kPairMaxUs) {
            spdlog::error("B09S-06 pair delay outside SRVT profile: pair={} us limit={} us", pair_delta, kPairMaxUs);
            return false;
        }
        pair_delta_us.push_back(pair_delta);

        if (i != 0U) {
            const long long cycle = std::chrono::duration_cast<std::chrono::microseconds>(
                                        normal.timestamp - previous_normal).count();
            if (cycle < kCycleMinUs || cycle > kCycleMaxUs) {
                spdlog::error("B09S-06 cycle outside protocol tolerance: cycle={} us expected={}..{} us",
                              cycle, kCycleMinUs, kCycleMaxUs);
                return false;
            }
            cycle_delta_us.push_back(cycle);
        }
        previous_normal = normal.timestamp;
    }

    const auto pair_minmax = std::minmax_element(pair_delta_us.begin(), pair_delta_us.end());
    const auto cycle_minmax = std::minmax_element(cycle_delta_us.begin(), cycle_delta_us.end());
    long long pair_sum = 0;
    long long cycle_sum = 0;
    for (const long long value : pair_delta_us) {
        pair_sum += value;
    }
    for (const long long value : cycle_delta_us) {
        cycle_sum += value;
    }
    const long long pair_avg = pair_sum / static_cast<long long>(pair_delta_us.size());
    const long long cycle_avg = cycle_sum / static_cast<long long>(cycle_delta_us.size());
    if (cycle_avg < kCycleAverageMinUs || cycle_avg > kCycleAverageMaxUs) {
        spdlog::error("B09S-06 average cycle inconsistent with SCT profile: avg={} us expected={}..{} us",
                      cycle_avg, kCycleAverageMinUs, kCycleAverageMaxUs);
        return false;
    }

    spdlog::info(
        "B09S-06 SocketCAN receive timestamp evidence: pairs={} intervals={} "
        "pair_us min/max/avg={}/{}/{} cycle_us min/max/avg={}/{}/{} "
        "configured_SCT_ms={}",
        pair_delta_us.size(), cycle_delta_us.size(), *pair_minmax.first, *pair_minmax.second, pair_avg,
        *cycle_minmax.first, *cycle_minmax.second, cycle_avg, kProfileSctMs);
    spdlog::info(
        "B09S-06 timing values are protocol/profile evidence only; "
        "they are not WCET or a safety timing budget");
    return waitForTxState(master, kStateEstablished);
}

bool validateResetRebind(CanopenTestMaster& master, SrdoWireFixture& fixture)
{
    SrdoDiagnostic before;
    if (!readDiagnostic(master, before) || before.tx_request_seq != before.tx_complete_seq) {
        spdlog::error("B09S-07 refuses reset with a pending TX request");
        return false;
    }
    if (!enterPreOperational(master) || !fixture.drain() || !resetCommunication(master)) {
        return false;
    }

    SrdoDiagnostic after;
    {
        /* Keep RX healthy while the SDO snapshot is collected. The safety wire is
         * not read in this scope, so the worker does not race the TX synchronizer. */
        ScopedRxPairWorker reset_snapshot_keepalive(fixture);
        const bool snapshot_ok = readDiagnostic(master, after);
        reset_snapshot_keepalive.stop();
        if (!snapshot_ok || !reset_snapshot_keepalive.ok()
            || after.tx_request_seq != before.tx_request_seq
            || after.tx_complete_seq != before.tx_complete_seq
            || after.tx_request_seq != after.tx_complete_seq
            || after.tx_request_result != kTxRequestResultIdle) {
            spdlog::error(
                "B09S-07 request state changed/replayed across reset: before={}/{} after={}/{} result={}",
                before.tx_request_seq, before.tx_complete_seq, after.tx_request_seq,
                after.tx_complete_seq, after.tx_request_result);
            return false;
        }
    }

    /* Transmission type 254 can resume natural cyclic SRDO TX immediately after
     * Operational. Observe complete natural pairs while synchronously refreshing
     * RX before its 100 ms SCT expires. Natural cyclic TX must not consume the
     * diagnostic request-result witness. */
    TxPairSynchronizer synchronizer(fixture, true);
    for (std::size_t i = 0U; i < kResetNaturalPairSamples; ++i) {
        WireFrame normal;
        WireFrame inverted;
        if (!synchronizer.nextPair(kWireTimeout, normal, inverted,
                                   "B09S-07 natural TX", true)
            || !validateWirePair(normal, inverted, kProbeNormal, "B09S-07 natural TX")) {
            return false;
        }
    }

    SrdoDiagnostic observed;
    {
        ScopedRxPairWorker reset_witness_keepalive(fixture);
        const bool witness_ok = readDiagnostic(master, observed);
        reset_witness_keepalive.stop();
        if (!witness_ok || !reset_witness_keepalive.ok()
            || observed.tx_request_seq != before.tx_request_seq
            || observed.tx_complete_seq != before.tx_complete_seq
            || observed.tx_request_seq != observed.tx_complete_seq
            || observed.tx_request_result != kTxRequestResultIdle) {
            spdlog::error(
                "B09S-07 stale diagnostic TX request was replayed while natural TX continued: "
                "request={}/{} result={}",
                observed.tx_request_seq, observed.tx_complete_seq, observed.tx_request_result);
            return false;
        }
    }

    if (!validateRxPair(master, fixture)
        || !validateEstablishedWindow(master, fixture, "B09S-07")
        || !validateTxPair(master, fixture)) {
        spdlog::error("B09S-07 reset/rebind validation failed");
        return false;
    }
    spdlog::info("B09S-07 Reset Communication rebind passed; natural periodic TX was not misclassified as stale replay");
    return true;
}

bool validateRegression(CanopenTestMaster& master, SrdoWireFixture& fixture)
{
    ScopedRxPairWorker keepalive(fixture);
    const int pdo_result = pdoProcess(master);
    const int sync_result = pdo_result == 0 ? syncPdoProcess(master) : 1;
    if (!keepalive.ok() || pdo_result != 0 || sync_result != 0) {
        spdlog::error("B09S-08 PDO/SYNC regression failed: keepalive={} pdo={} sync={}",
                      keepalive.ok(), pdo_result, sync_result);
        return false;
    }

    std::uint32_t device_type = 0U;
    if (!readRemote(master, 0x1000U, 0x00U, device_type, "B09S-08 0x1000 health read")) {
        return false;
    }
    spdlog::info("B09S-08 PDO/SYNC/SDO/NMT regression passed");
    return true;
}

bool validateF01(CanopenTestMaster& master, SrdoWireFixture& fixture, const SrdoProfile& baseline)
{
    if (!prepareCleanOperational(master, baseline) || !fixture.sendPair(kProbeNormal, kProbeNormal)
        || !waitForRxState(master, kStateRxNotInverted)) {
        spdlog::error("B09S-F01 wrong inverse was not detected");
        return false;
    }
    spdlog::info("B09S-F01 wrong inverse detected as state -2");
    return true;
}

bool validateF02(CanopenTestMaster& master, SrdoWireFixture& fixture, const SrdoProfile& baseline)
{
    if (!prepareCleanOperational(master, baseline)) {
        return false;
    }
    const std::array<std::uint8_t, kPayloadLength> normal = {{0x78U, 0x56U, 0x34U, 0x12U}};
    if (!fixture.sendRx(kRxNormalCanId, normal.data(), kPayloadLength)) {
        return false;
    }
    std::this_thread::sleep_for(kSrvtFaultWait);
    if (!waitForRxState(master, kStateRxTimeoutSrvt, std::chrono::milliseconds(500))) {
        spdlog::error("B09S-F02 missing inverted frame did not reach SRVT timeout");
        return false;
    }
    spdlog::info("B09S-F02 missing inverted frame detected as state -4");
    return true;
}

bool validateF03(CanopenTestMaster& master, SrdoWireFixture& fixture, const SrdoProfile& baseline)
{
    if (!prepareCleanOperational(master, baseline) || !fixture.sendPair(kProbeNormal, kProbeInverted)
        || !waitForRxState(master, kStateEstablished)) {
        return false;
    }
    std::this_thread::sleep_for(kSctFaultWait);
    if (!waitForRxState(master, kStateRxTimeoutSct, std::chrono::milliseconds(500))) {
        spdlog::error("B09S-F03 missing complete pair did not reach SCT timeout");
        return false;
    }
    spdlog::info("B09S-F03 missing complete pair detected as state -3");
    return true;
}

bool validateF04(CanopenTestMaster& master, SrdoWireFixture& fixture, const SrdoProfile& baseline)
{
    if (!prepareCleanOperational(master, baseline)) {
        return false;
    }
    const std::uint8_t short_payload[2] = {0x78U, 0x56U};
    if (!fixture.sendRx(kRxNormalCanId, short_payload, 2U) || !waitForRxState(master, kStateRxShort)) {
        spdlog::error("B09S-F04 short RX frame was not detected");
        return false;
    }
    spdlog::info("B09S-F04 short RX detected as state -1");
    return true;
}

bool validateF05(CanopenTestMaster& master, SrdoWireFixture& fixture, const SrdoProfile& baseline)
{
    if (!prepareCleanOperational(master, baseline)) {
        return false;
    }
    const std::array<std::uint8_t, kPayloadLength> inverted = {{0x87U, 0xA9U, 0xCBU, 0xEDU}};
    if (!fixture.sendRx(kRxInvertedCanId, inverted.data(), kPayloadLength)) {
        return false;
    }

    const Deadline inverted_first_deadline =
        SteadyClock::now() + kSctFaultWait + std::chrono::milliseconds(500);
    bool inverted_first_timed_out = false;
    while (SteadyClock::now() < inverted_first_deadline) {
        std::int8_t rx_state = 0;
        const Deadline sample_deadline = std::min(
            inverted_first_deadline, SteadyClock::now() + kF05StateSampleBudget);
        if (!readRemoteUntil(master, kSrdoDiagnosticIndex, 0x06U, rx_state,
                             "F05-A rx_state", sample_deadline)) {
            spdlog::error(
                "B09S-F05-A RX state sample exceeded {} ms observation budget",
                kF05StateSampleBudget.count());
            return false;
        }
        if (rx_state == kStateEstablished) {
            spdlog::error("B09S-F05-A inverted-first was temporarily accepted as established");
            return false;
        }
        if (rx_state == kStateRxTimeoutSct) {
            inverted_first_timed_out = true;
            break;
        }
        std::this_thread::sleep_for(
            std::min(kDiagnosticPoll, remainingBudget(inverted_first_deadline)));
    }
    if (!inverted_first_timed_out) {
        spdlog::error("B09S-F05-A inverted-first did not end in SCT timeout");
        return false;
    }

    if (!prepareCleanOperational(master, baseline)) {
        return false;
    }
    const std::array<std::uint8_t, kPayloadLength> normal = {{0x78U, 0x56U, 0x34U, 0x12U}};
    if (!fixture.sendRx(kRxNormalCanId, normal.data(), kPayloadLength)) {
        return false;
    }
    std::this_thread::sleep_for(kRxPairDelay);
    if (!fixture.sendRx(kRxNormalCanId, normal.data(), kPayloadLength)) {
        return false;
    }
    std::this_thread::sleep_for(kSrvtFaultWait);
    if (!waitForRxState(master, kStateRxTimeoutSrvt, std::chrono::milliseconds(500))) {
        spdlog::error("B09S-F05-B duplicate-normal did not end in SRVT timeout");
        return false;
    }
    spdlog::info("B09S-F05 wrong-order cases passed");
    return true;
}

bool validateF06(CanopenTestMaster& master, const SrdoProfile& baseline)
{
    if (!prepareCleanOperational(master, baseline) || !enterPreOperational(master)) {
        return false;
    }
    const std::uint16_t wrong_checksum = static_cast<std::uint16_t>(baseline.checksum[0] ^ 0x0001U);
    if (!writeRemote(master, kSrdoChecksumIndex, 0x01U, wrong_checksum, "F06 wrong SRDO1 checksum")
        || !writeRemote(master, kSrdoConfigValidIndex, 0x00U, static_cast<std::uint8_t>(0xA5U), "F06 valid magic")
        || !enterOperational(master) || !waitForRxState(master, kStateErrorConfiguration)) {
        spdlog::error("B09S-F06 checksum mismatch did not cause configuration error");
        return false;
    }
    std::uint8_t valid = 0xFFU;
    if (!readRemote(master, kSrdoConfigValidIndex, 0x00U, valid, "F06 0x13FE after fault") || valid != 0U) {
        spdlog::error("B09S-F06 expected 0x13FE=0 after fault, actual=0x{:02x}", static_cast<unsigned int>(valid));
        return false;
    }
    spdlog::info("B09S-F06 checksum mismatch passed");
    return true;
}

bool validateF07(CanopenTestMaster& master, const SrdoProfile& baseline)
{
    if (!prepareCleanOperational(master, baseline) || !enterPreOperational(master)) {
        return false;
    }
    const std::uint16_t changed_sct = static_cast<std::uint16_t>(baseline.comm[0].sct_ms + 1U);
    std::uint8_t valid = 0xFFU;
    if (!writeRemote(master, kSrdo1CommIndex, 0x02U, changed_sct, "F07 legal SCT change")
        || !readRemote(master, kSrdoConfigValidIndex, 0x00U, valid, "F07 0x13FE after parameter write")
        || valid != 0U) {
        spdlog::error("B09S-F07 parameter write did not clear 0x13FE");
        return false;
    }
    if (!writeRemote(master, kSrdo1CommIndex, 0x02U, baseline.comm[0].sct_ms, "F07 restore SCT")
        || !writeRemote(master, kSrdoChecksumIndex, 0x01U, baseline.checksum[0], "F07 restore checksum")
        || !writeRemote(master, kSrdoConfigValidIndex, 0x00U, static_cast<std::uint8_t>(0xA5U), "F07 restore valid")
        || !enterOperational(master)) {
        return false;
    }
    spdlog::info("B09S-F07 configuration-valid invalidation passed");
    return true;
}

bool validateF08(CanopenTestMaster& master, const SrdoProfile& baseline)
{
    if (!prepareCleanOperational(master, baseline) || !enterPreOperational(master)) {
        return false;
    }
    if (!requireWriteRejected(master, kSrdo1CommIndex, 0x05U, static_cast<std::uint32_t>(0x102U),
                              lely::canopen::SdoErrc::PARAM_VAL, "F08 normal CAN-ID even")
        || !requireWriteRejected(master, kSrdo1CommIndex, 0x06U, static_cast<std::uint32_t>(0x103U),
                                 lely::canopen::SdoErrc::PARAM_VAL, "F08 inverted CAN-ID odd")
        || !requireWriteRejected(master, kSrdo1CommIndex, 0x05U, static_cast<std::uint32_t>(0x181U),
                                 lely::canopen::SdoErrc::PARAM_VAL, "F08 CAN-ID out of range")) {
        return false;
    }
    std::uint32_t current_normal = 0U;
    std::uint32_t current_inverted = 0U;
    if (!readRemote(master, kSrdo1CommIndex, 0x05U, current_normal, "F08 preserved normal CAN-ID")
        || !readRemote(master, kSrdo1CommIndex, 0x06U, current_inverted, "F08 preserved inverted CAN-ID")
        || current_normal != baseline.comm[0].normal_id || current_inverted != baseline.comm[0].inverted_id) {
        spdlog::error("B09S-F08 rejected write changed CAN-ID pair");
        return false;
    }

    SrdoCommProfile nonconsecutive = baseline.comm[0];
    nonconsecutive.inverted_id = 0x106U;
    const std::uint16_t checksum = calculateSrdoChecksum(nonconsecutive, baseline.map[0]);
    if (!writeRemote(master, kSrdo1CommIndex, 0x06U, nonconsecutive.inverted_id,
                     "F08 non-consecutive inverted CAN-ID")
        || !writeRemote(master, kSrdoChecksumIndex, 0x01U, checksum, "F08 checksum for non-consecutive pair")
        || !writeRemote(master, kSrdoConfigValidIndex, 0x00U, static_cast<std::uint8_t>(0xA5U), "F08 valid magic")
        || !enterOperational(master) || !waitForRxState(master, kStateErrorConfiguration)) {
        spdlog::error("B09S-F08 non-consecutive pair did not fail configuration");
        return false;
    }
    spdlog::info("B09S-F08 invalid COB-ID checks passed");
    return true;
}

bool validateF09(CanopenTestMaster& master, const SrdoProfile& baseline)
{
    if (!prepareCleanOperational(master, baseline) || !enterPreOperational(master)) {
        return false;
    }
    if (!writeRemote(master, kSrdo1CommIndex, 0x01U, static_cast<std::uint8_t>(0U), "F09 disable SRDO1")
        || !requireWriteRejected(master, kSrdo1MapIndex, 0x00U, static_cast<std::uint8_t>(1U),
                                 lely::canopen::SdoErrc::PDO_LEN, "F09 odd mapping count")) {
        return false;
    }
    std::uint8_t map_count = 0U;
    if (!readRemote(master, kSrdo1MapIndex, 0x00U, map_count, "F09 preserved mapping count")
        || map_count != baseline.map[0].count) {
        spdlog::error("B09S-F09 rejected odd count changed mapping count");
        return false;
    }
    if (!writeRemote(master, kSrdo1CommIndex, 0x01U, baseline.comm[0].direction, "F09 restore direction")
        || !writeRemote(master, kSrdoChecksumIndex, 0x01U, baseline.checksum[0], "F09 restore checksum")
        || !writeRemote(master, kSrdoConfigValidIndex, 0x00U, static_cast<std::uint8_t>(0xA5U), "F09 restore valid")
        || !enterOperational(master)) {
        return false;
    }
    spdlog::info("B09S-F09 odd mapping count rejection passed");
    return true;
}

bool validateF10(CanopenTestMaster& master, const SrdoProfile& baseline)
{
    if (!prepareCleanOperational(master, baseline) || !enterPreOperational(master)) {
        return false;
    }
    SrdoMapProfile mismatched = baseline.map[0];
    mismatched.entry[1] = 0x23060210UL;
    const std::uint16_t checksum = calculateSrdoChecksum(baseline.comm[0], mismatched);
    if (!writeRemote(master, kSrdo1CommIndex, 0x01U, static_cast<std::uint8_t>(0U), "F10 disable SRDO1")
        || !writeRemote(master, kSrdo1MapIndex, 0x00U, static_cast<std::uint8_t>(0U), "F10 clear map count")
        || !writeRemote(master, kSrdo1MapIndex, 0x01U, mismatched.entry[0], "F10 normal 32-bit map")
        || !writeRemote(master, kSrdo1MapIndex, 0x02U, mismatched.entry[1], "F10 inverted 16-bit map")
        || !writeRemote(master, kSrdo1MapIndex, 0x00U, mismatched.count, "F10 restore map count")
        || !writeRemote(master, kSrdo1CommIndex, 0x01U, baseline.comm[0].direction, "F10 restore direction")
        || !writeRemote(master, kSrdoChecksumIndex, 0x01U, checksum, "F10 checksum for mismatched map")
        || !writeRemote(master, kSrdoConfigValidIndex, 0x00U, static_cast<std::uint8_t>(0xA5U), "F10 valid magic")
        || !enterOperational(master) || !waitForRxState(master, kStateErrorConfiguration)) {
        spdlog::error("B09S-F10 mapping length mismatch did not fail configuration");
        return false;
    }
    std::uint8_t valid = 0xFFU;
    if (!readRemote(master, kSrdoConfigValidIndex, 0x00U, valid, "F10 0x13FE after fault") || valid != 0U) {
        spdlog::error("B09S-F10 expected 0x13FE=0 after fault");
        return false;
    }
    spdlog::info("B09S-F10 mapping length mismatch passed");
    return true;
}

bool validateF11(CanopenTestMaster& master, SrdoWireFixture& fixture, const SrdoProfile& baseline)
{
    /* Quiesce SRDO TX before establishing the evidence boundary. Drain only
     * after Pre-operational is confirmed, while cyclic TX is disabled. */
    if (!prepareCleanOperational(master, baseline)
        || !enterPreOperational(master)
        || !fixture.drain()
        || !writeRemote(master, kSrdoDiagnosticIndex, 0x03U, kProbeNormal, "F11 tx_normal")
        || !writeRemote(master, kSrdoDiagnosticIndex, 0x04U, kWrongInverted, "F11 wrong tx_inverted")) {
        return false;
    }

    /* From this point through state -6 and the post-fault hold, one dedicated
     * reader continuously owns the safety-wire channel. There is deliberately
     * no post-fault drain, so any 0x103/0x104 emission is a hard failure. */
    ScopedTxSilenceObserver observer(fixture);
    if (!enterOperational(master)) {
        return false;
    }

    const Deadline state_deadline = SteadyClock::now() + kStateTimeout;
    bool fault_observed = false;
    while (SteadyClock::now() < state_deadline) {
        if (!observer.ok()) {
            return false;
        }
        const std::uint32_t unexpected_id = observer.unexpectedId();
        if (unexpected_id != 0U) {
            spdlog::error(
                "B09S-F11 emitted SRDO frame after fault activation before state -6: id=0x{:03x}",
                unexpected_id);
            return false;
        }

        std::int8_t tx_state = 0;
        if (!readRemoteUntil(master, kSrdoDiagnosticIndex, 0x07U, tx_state,
                             "F11 tx_state", state_deadline)) {
            return false;
        }
        if (tx_state == kStateTxNotInverted) {
            fault_observed = true;
            break;
        }
        if (tx_state < 0) {
            spdlog::error("B09S-F11 unexpected TX state before -6: actual={}",
                          static_cast<int>(tx_state));
            return false;
        }
        std::this_thread::sleep_for(std::min(kDiagnosticPoll, remainingBudget(state_deadline)));
    }

    if (!fault_observed) {
        spdlog::error("B09S-F11 TX inversion check did not reach state -6");
        return false;
    }

    const Deadline silence_deadline = SteadyClock::now() + kNoTxObservation;
    while (SteadyClock::now() < silence_deadline) {
        if (!observer.ok()) {
            return false;
        }
        const std::uint32_t unexpected_id = observer.unexpectedId();
        if (unexpected_id != 0U) {
            spdlog::error("B09S-F11 emitted SRDO frame after TX inversion failure: id=0x{:03x}",
                          unexpected_id);
            return false;
        }
        std::this_thread::sleep_for(std::min(kDiagnosticPoll, remainingBudget(silence_deadline)));
    }

    observer.stop();
    if (!observer.ok()) {
        return false;
    }
    const std::uint32_t unexpected_id = observer.unexpectedId();
    if (unexpected_id != 0U) {
        spdlog::error("B09S-F11 emitted SRDO frame at silence-window boundary: id=0x{:03x}",
                      unexpected_id);
        return false;
    }

    spdlog::info("B09S-F11 TX data check failure passed: state=-6 and no wire emission");
    return true;
}

bool validateF12(CanopenTestMaster& master, SrdoWireFixture& fixture, const SrdoProfile& baseline)
{
    /* F12 is specifically repair + communication reset + sustained valid RX
     * traffic -> state 3. B09S-05 already covers the explicit TX request path,
     * so do not place unrelated SDO/TX work before the recovery SCT window. */
    if (!prepareCleanOperational(master, baseline)
        || !validateEstablishedWindow(master, fixture, "B09S-F12")) {
        spdlog::error("B09S-F12 final recovery failed");
        return false;
    }
    spdlog::info("B09S-F12 final recovery passed");
    return true;
}

} // namespace

int srdoProcess(CanopenTestMaster& master, lely::io::CanChannel& wire_channel)
{
    spdlog::info("Starting J09/B09S SRDO protocol validation");
    SrdoWireFixture fixture(wire_channel);
    SrdoProfile baseline;

    /* B09G and B09S intentionally share one safety wire channel as a sequential
     * test resource. Start by draining frames left by any preceding safety stage. */
    if (!fixture.drain() || !validateNmtNonOperational(master) || !validateProfile(master, baseline)) {
        spdlog::error("J09/B09S SRDO protocol validation FAILED");
        return 1;
    }

    const bool passed = enterOperational(master)
        && validateRxPair(master, fixture)
        && validateEstablishedWindow(master, fixture, "B09S-01")
        && validateTxPair(master, fixture)
        && validateTxCycles(master, fixture)
        && validateResetRebind(master, fixture)
        && validateRegression(master, fixture)
        && validateF01(master, fixture, baseline)
        && validateF02(master, fixture, baseline)
        && validateF03(master, fixture, baseline)
        && validateF04(master, fixture, baseline)
        && validateF05(master, fixture, baseline)
        && validateF06(master, baseline)
        && validateF07(master, baseline)
        && validateF08(master, baseline)
        && validateF09(master, baseline)
        && validateF10(master, baseline)
        && validateF11(master, fixture, baseline)
        && validateF12(master, fixture, baseline);

    if (!passed) {
        if (!prepareCleanOperational(master, baseline)) {
            spdlog::error("B09S could not restore the SRDO baseline after validation failure");
        }
        spdlog::error("J09/B09S SRDO protocol validation FAILED");
        return 1;
    }

    spdlog::info(
        "CANopenNode GFC/SRDO protocol functional verification PASS on the tested STM32F407/RT-Thread configuration.");
    return 0;
}
