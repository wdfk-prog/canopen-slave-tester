/**
 * @file
 * @brief Implements synchronized runtime state transactions and formatting.
 */

#include "canopen_test/runtime_status.hpp"

#include <utility>

namespace canopen_test {

void RuntimeStatus::ResetForStart()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t next_session_id = status_.session_id + 1U;
    status_ = RuntimeStatusSnapshot{};
    status_.session_id = next_session_id;
    status_.lifecycle = RuntimeLifecycle::kStarting;
}

void RuntimeStatus::SetLifecycle(RuntimeLifecycle lifecycle)
{
    std::lock_guard<std::mutex> lock(mutex_);
    status_.lifecycle = lifecycle;
}

void RuntimeStatus::SetCanStateEvent(RuntimeCanState state, std::string event)
{
    std::lock_guard<std::mutex> lock(mutex_);
    status_.can_state = state;
    status_.last_event = std::move(event);
}

void RuntimeStatus::RecordCanError(std::string event)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++status_.can_error_count;
    status_.last_event = std::move(event);
}

void RuntimeStatus::SetLastEvent(std::string event)
{
    std::lock_guard<std::mutex> lock(mutex_);
    status_.last_event = std::move(event);
}

void RuntimeStatus::SetLastException(std::string exception)
{
    std::lock_guard<std::mutex> lock(mutex_);
    status_.lifecycle = RuntimeLifecycle::kFailed;
    status_.last_exception = std::move(exception);
}

RuntimeStatusSnapshot RuntimeStatus::Snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

const char* ToString(RuntimeLifecycle lifecycle) noexcept
{
    switch (lifecycle) {
    case RuntimeLifecycle::kCreated:
        return "created";
    case RuntimeLifecycle::kStarting:
        return "starting";
    case RuntimeLifecycle::kReady:
        return "ready";
    case RuntimeLifecycle::kRunning:
        return "running";
    case RuntimeLifecycle::kStopping:
        return "stopping";
    case RuntimeLifecycle::kStopped:
        return "stopped";
    case RuntimeLifecycle::kFailed:
        return "failed";
    }
    return "unknown";
}

const char* ToString(RuntimeCanState state) noexcept
{
    switch (state) {
    case RuntimeCanState::kUnknown:
        return "unknown";
    case RuntimeCanState::kActive:
        return "active";
    case RuntimeCanState::kPassive:
        return "passive";
    case RuntimeCanState::kBusOff:
        return "bus-off";
    case RuntimeCanState::kSleeping:
        return "sleeping";
    case RuntimeCanState::kStopped:
        return "stopped";
    }
    return "unknown";
}

} // namespace canopen_test
