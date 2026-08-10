/**
 * @file
 * @brief Common remote SDO read/write helpers for CANopen test processes.
 *
 * SDO-specific configuration belongs here rather than in canopen_config.h so
 * the global configuration header remains limited to cross-module settings.
 */

#ifndef CANOPEN_SDO_H
#define CANOPEN_SDO_H

/** Default protocol-level timeout for remote SDO transactions. */
#define CANOPEN_SDO_TIMEOUT_MS 5000
/** Extra local wait for an SDO completion callback after protocol timeout. */
#define CANOPEN_SDO_COMPLETION_MARGIN_MS 500

#include <lely/coapp/master.hpp>
#include <lely/coapp/sdo_error.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <system_error>

/**
 * @brief Result classification for one remote SDO operation.
 *
 * The distinction between SDO_TIMEOUT and WAIT_TIMEOUT is intentional:
 * SDO_TIMEOUT means Lely completed the request and the SDO channel state is
 * known, while WAIT_TIMEOUT means the local waiter never observed completion
 * and the remote transaction state must be treated as unknown.
 */
enum class SdoOperationResult {
    SUCCESS, /**< Remote transaction completed successfully. */
    FAILED, /**< Submission or non-timeout SDO completion failed. */
    SDO_TIMEOUT, /**< Lely reported a protocol-level SDO timeout. */
    WAIT_TIMEOUT, /**< Local callback wait expired before completion. */
};

namespace canopen_sdo_detail {

/**
 * @brief Shared state for one asynchronous SDO upload.
 *
 * @tparam T Value type returned by the remote object dictionary entry.
 */
template <class T>
struct ReadState {
    std::mutex mutex; /**< Protects all fields published by the callback. */
    std::condition_variable condition; /**< Wakes the synchronous process waiter. */
    bool completed = false; /**< Safe initial state: no callback has completed yet. */
    std::error_code error; /**< Default success until the callback reports an error. */
    T value{}; /**< Value-initialized placeholder overwritten on successful upload. */
};

/** Shared state for one asynchronous SDO download. */
struct WriteState {
    std::mutex mutex; /**< Protects callback completion and error fields. */
    std::condition_variable condition; /**< Wakes the synchronous process waiter. */
    bool completed = false; /**< Safe initial state: no callback has completed yet. */
    std::error_code error; /**< Default success until the callback reports an error. */
};

} // namespace canopen_sdo_detail

/**
 * @brief Read one object from a remote CANopen node through SDO.
 *
 * SubmitRead() remains asynchronous on the Lely executor. This helper blocks
 * only the caller/process thread while waiting for the completion callback;
 * it must not be called from the same event-loop thread that executes Lely
 * callbacks.
 *
 * @tparam T CANopen object value type accepted by Lely SubmitRead().
 * @param master Active Lely asynchronous CANopen master.
 * @param node_id Remote node-ID whose SDO server is accessed.
 * @param index Remote object dictionary index.
 * @param subindex Remote object dictionary sub-index.
 * @param value Receives the uploaded value only on SUCCESS.
 * @param sdo_timeout Protocol-level timeout passed to Lely.
 * @param completion_margin Extra local wait after the protocol timeout so a
 *        queued completion callback can still be observed.
 * @return Operation result including protocol and local completion timeouts.
 */
template <class T>
SdoOperationResult readRemoteSdo(
    lely::canopen::AsyncMaster& master, std::uint8_t node_id,
    std::uint16_t index, std::uint8_t subindex, T& value,
    std::chrono::milliseconds sdo_timeout =
        std::chrono::milliseconds(CANOPEN_SDO_TIMEOUT_MS),
    std::chrono::milliseconds completion_margin =
        std::chrono::milliseconds(CANOPEN_SDO_COMPLETION_MARGIN_MS))
{
    /* Shared ownership keeps request state alive until callback dispatch even
     * if the process-side wait has already returned. */
    const auto state = std::make_shared<canopen_sdo_detail::ReadState<T>>();
    /* Default success lets SubmitRead report immediate scheduling/setup
     * failure separately from the later protocol completion result. */
    std::error_code submit_error;

    master.SubmitRead<T>(
        master.GetExecutor(), node_id, index, subindex,
        [state](std::uint8_t, std::uint16_t, std::uint8_t,
                std::error_code error, T read_value) noexcept {
            {
                /* Publish the entire completion snapshot before waking the
                 * waiting process thread. */
                std::lock_guard<std::mutex> lock(state->mutex);
                state->completed = true;
                state->error = error;
                state->value = read_value;
            }
            state->condition.notify_all();
        },
        sdo_timeout, submit_error);

    if (submit_error) {
        spdlog::error(
            "Unable to submit SDO read: node={} object=0x{:04x}:{:02x}: {}",
            static_cast<unsigned int>(node_id), index,
            static_cast<unsigned int>(subindex), submit_error.message());
        return SdoOperationResult::FAILED;
    }

    /* The process thread may block here because the Lely event loop runs on a
     * separate thread and remains free to complete the SDO transaction. */
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(
            lock, sdo_timeout + completion_margin,
            [state]() { return state->completed; })) {
        spdlog::error(
            "SDO read completion timed out: node={} object=0x{:04x}:{:02x}; "
            "remote transaction state is unknown",
            static_cast<unsigned int>(node_id), index,
            static_cast<unsigned int>(subindex));
        return SdoOperationResult::WAIT_TIMEOUT;
    }

    if (state->error) {
        if (lely::canopen::sdo_errc(state->error)
            == lely::canopen::SdoErrc::TIMEOUT) {
            spdlog::error(
                "SDO read timed out: node={} object=0x{:04x}:{:02x}: {}",
                static_cast<unsigned int>(node_id), index,
                static_cast<unsigned int>(subindex), state->error.message());
            return SdoOperationResult::SDO_TIMEOUT;
        }
        spdlog::error("SDO read failed: node={} object=0x{:04x}:{:02x}: {}",
                      static_cast<unsigned int>(node_id), index,
                      static_cast<unsigned int>(subindex),
                      state->error.message());
        return SdoOperationResult::FAILED;
    }

    /* Do not modify the caller's output until the remote transaction has
     * completed successfully. */
    value = state->value;
    return SdoOperationResult::SUCCESS;
}

/**
 * @brief Write one object to a remote CANopen node through SDO.
 *
 * SubmitWrite() remains asynchronous on the Lely executor. This helper blocks
 * only the caller/process thread while waiting for the completion callback;
 * it must not be called from the same event-loop thread that executes Lely
 * callbacks.
 *
 * @tparam T CANopen object value type accepted by Lely SubmitWrite().
 * @param master Active Lely asynchronous CANopen master.
 * @param node_id Remote node-ID whose SDO server is accessed.
 * @param index Remote object dictionary index.
 * @param subindex Remote object dictionary sub-index.
 * @param value Value downloaded to the remote object.
 * @param sdo_timeout Protocol-level timeout passed to Lely.
 * @param completion_margin Extra local wait after the protocol timeout so a
 *        queued completion callback can still be observed.
 * @return Operation result including protocol and local completion timeouts.
 */
template <class T>
SdoOperationResult writeRemoteSdo(
    lely::canopen::AsyncMaster& master, std::uint8_t node_id,
    std::uint16_t index, std::uint8_t subindex, T value,
    std::chrono::milliseconds sdo_timeout =
        std::chrono::milliseconds(CANOPEN_SDO_TIMEOUT_MS),
    std::chrono::milliseconds completion_margin =
        std::chrono::milliseconds(CANOPEN_SDO_COMPLETION_MARGIN_MS))
{
    /* Shared ownership keeps callback state valid through asynchronous
     * completion or a process-side wait timeout. */
    const auto state = std::make_shared<canopen_sdo_detail::WriteState>();
    /* Default success lets SubmitWrite report immediate submission errors
     * separately from the later SDO completion result. */
    std::error_code submit_error;

    master.SubmitWrite(
        master.GetExecutor(), node_id, index, subindex, value,
        [state](std::uint8_t, std::uint16_t, std::uint8_t,
                std::error_code error) noexcept {
            {
                /* Publish completion and error atomically before notification. */
                std::lock_guard<std::mutex> lock(state->mutex);
                state->completed = true;
                state->error = error;
            }
            state->condition.notify_all();
        },
        sdo_timeout, submit_error);

    if (submit_error) {
        spdlog::error(
            "Unable to submit SDO write: node={} object=0x{:04x}:{:02x}: {}",
            static_cast<unsigned int>(node_id), index,
            static_cast<unsigned int>(subindex), submit_error.message());
        return SdoOperationResult::FAILED;
    }

    /* Wait off the event-loop thread so Lely can continue asynchronous I/O. */
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(
            lock, sdo_timeout + completion_margin,
            [state]() { return state->completed; })) {
        spdlog::error(
            "SDO write completion timed out: node={} object=0x{:04x}:{:02x}; "
            "remote transaction state is unknown",
            static_cast<unsigned int>(node_id), index,
            static_cast<unsigned int>(subindex));
        return SdoOperationResult::WAIT_TIMEOUT;
    }

    if (state->error) {
        if (lely::canopen::sdo_errc(state->error)
            == lely::canopen::SdoErrc::TIMEOUT) {
            spdlog::error(
                "SDO write timed out: node={} object=0x{:04x}:{:02x}: {}",
                static_cast<unsigned int>(node_id), index,
                static_cast<unsigned int>(subindex), state->error.message());
            return SdoOperationResult::SDO_TIMEOUT;
        }
        spdlog::error("SDO write failed: node={} object=0x{:04x}:{:02x}: {}",
                      static_cast<unsigned int>(node_id), index,
                      static_cast<unsigned int>(subindex),
                      state->error.message());
        return SdoOperationResult::FAILED;
    }

    return SdoOperationResult::SUCCESS;
}

#endif /* CANOPEN_SDO_H */
