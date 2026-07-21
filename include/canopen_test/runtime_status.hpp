/**
 * @file
 * @brief Declares synchronized runtime lifecycle and CAN health state.
 */

#ifndef CANOPEN_TEST_RUNTIME_STATUS_HPP_
#define CANOPEN_TEST_RUNTIME_STATUS_HPP_

#include <cstdint>
#include <mutex>
#include <string>

namespace canopen_test {

/** @brief Lifecycle state of the Lely runtime. */
enum class RuntimeLifecycle {
    kCreated,
    kStarting,
    kReady,
    kRunning,
    kStopping,
    kStopped,
    kFailed,
};

/** @brief CAN controller state independent of Lely public headers. */
enum class RuntimeCanState {
    kUnknown,
    kActive,
    kPassive,
    kBusOff,
    kSleeping,
    kStopped,
};

/**
 * @brief Point-in-time copy of all synchronized runtime health fields.
 *
 * RuntimeStatus copies this complete structure while holding one state mutex.
 * Fields belonging to one logical callback event are committed through compound
 * recorder methods so readers cannot observe a half-applied event.
 */
struct RuntimeStatusSnapshot {
    /** Monotonically increasing identifier for each Start() attempt. */
    std::uint64_t session_id{0};
    /** Current runtime lifecycle state. */
    RuntimeLifecycle lifecycle{RuntimeLifecycle::kCreated};
    /** Most recently observed CAN controller state. */
    RuntimeCanState can_state{RuntimeCanState::kUnknown};
    /** Most recent asynchronous runtime event in the current session. */
    std::string last_event;
    /** Most recent failure in the current session; non-empty implies failure. */
    std::string last_exception;
    /** Number of CAN error callbacks observed in the current session. */
    std::uint64_t can_error_count{0};
};

/**
 * @brief Serializes complete runtime state with one short-lived mutex.
 *
 * A regular mutex is intentional. The protected state contains strings whose
 * assignment can allocate memory, so a spin lock could waste CPU if the owner
 * is preempted. Exposing only domain-specific compound updates also prevents
 * callers from holding the mutex while invoking Lely or logging APIs.
 */
class RuntimeStatus final {
public:
    /** @brief Starts a new session and clears all previous-session state. */
    void ResetForStart();

    /**
     * @brief Updates the runtime lifecycle state.
     * @param lifecycle New lifecycle state.
     */
    void SetLifecycle(RuntimeLifecycle lifecycle);

    /**
     * @brief Atomically updates the CAN state and its associated event.
     * @param state New CAN controller state.
     * @param event Event describing the state transition.
     */
    void SetCanStateEvent(RuntimeCanState state, std::string event);

    /**
     * @brief Records one CAN error and its associated event atomically.
     * @param event CAN error description.
     */
    void RecordCanError(std::string event);

    /**
     * @brief Stores the most recent non-CAN event description.
     * @param event Event description.
     */
    void SetLastEvent(std::string event);

    /**
     * @brief Stores the most recent exception and marks the session failed.
     * @param exception Exception description.
     */
    void SetLastException(std::string exception);

    /**
     * @brief Captures all runtime fields while holding the state mutex.
     * @return Point-in-time copy of the complete runtime state.
     */
    RuntimeStatusSnapshot Snapshot() const;

private:
    mutable std::mutex mutex_;      /**< Protects every field in status_. */
    RuntimeStatusSnapshot status_;  /**< Complete state committed under mutex_. */
};

/**
 * @brief Converts a lifecycle state to a stable display string.
 * @param lifecycle Lifecycle state.
 * @return Static display string.
 */
const char* ToString(RuntimeLifecycle lifecycle) noexcept;

/**
 * @brief Converts a CAN state to a stable display string.
 * @param state CAN state.
 * @return Static display string.
 */
const char* ToString(RuntimeCanState state) noexcept;

} // namespace canopen_test

#endif /* CANOPEN_TEST_RUNTIME_STATUS_HPP_ */
