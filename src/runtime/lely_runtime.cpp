/**
 * @file
 * @brief Implements Lely I/O, SocketCAN, AsyncMaster, and shutdown management.
 */

#include "canopen_test/lely_runtime.hpp"

#include "canopen_test/error.hpp"
#include "canopen_test/logger.hpp"

#include <lely/coapp/master.hpp>
#include <lely/ev/exec.hpp>
#include <lely/ev/loop.hpp>
#include <lely/io2/ctx.hpp>
#include <lely/io2/linux/can.hpp>
#include <lely/io2/posix/poll.hpp>
#include <lely/io2/sys/io.hpp>
#include <lely/io2/sys/timer.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

namespace canopen_test {
namespace {

/**
 * @brief Maps Lely CAN state values to the dependency-free runtime model.
 *
 * @param state Lely CAN controller state.
 * @return Equivalent runtime state or kUnknown.
 */
RuntimeCanState ConvertCanState(lely::io::CanState state) noexcept
{
    switch (state) {
    case lely::io::CanState::ACTIVE:
        return RuntimeCanState::kActive;
    case lely::io::CanState::PASSIVE:
        return RuntimeCanState::kPassive;
    case lely::io::CanState::BUSOFF:
        return RuntimeCanState::kBusOff;
    case lely::io::CanState::SLEEPING:
        return RuntimeCanState::kSleeping;
    case lely::io::CanState::STOPPED:
        return RuntimeCanState::kStopped;
    }
    return RuntimeCanState::kUnknown;
}

/**
 * @brief Builds a consistent operation failure description.
 *
 * @param operation Operation that failed.
 * @param exception Captured exception.
 * @return Combined diagnostic text.
 */
std::string SystemErrorMessage(const std::string& operation, const std::exception& exception)
{
    return operation + " failed: " + exception.what();
}

} // namespace

/**
 * @brief Private runtime state keeping Lely headers out of the public interface.
 *
 * Resource declarations follow their dependency order. Stop() resets them in
 * reverse order so no object outlives an object it references.
 */
struct LelyRuntime::Impl {
    explicit Impl(RuntimeStatus& runtime_status)
        : status(runtime_status)
    {
    }

    /** Caller-owned thread-safe status recorder. */
    RuntimeStatus& status;
    /** Validated configuration copied at startup. */
    AppConfig config;
    /** Serializes lifecycle flags and resource access across control threads. */
    std::mutex control_mutex;
    /** Wakes Stop() after Run() relinquishes event-loop resources. */
    std::condition_variable control_condition;
    /** Thread currently executing Loop::run(), or the default ID when idle. */
    std::thread::id run_thread_id;
    /** True after Start() completes successfully. */
    bool started{false};
    /** True while Loop::run() is active. */
    bool running{false};
    /** Latched stop request accepted from any normal thread. */
    bool stop_requested{false};
    /** Prevents duplicate shutdown tasks from being posted. */
    bool stop_posted{false};

    /** Keeps the global Lely I/O subsystem initialized. */
    std::unique_ptr<lely::io::IoGuard> io_guard;
    /** Root Lely I/O context. */
    std::unique_ptr<lely::io::Context> context;
    /** POSIX poll backend associated with context. */
    std::unique_ptr<lely::io::Poll> poll;
    /** Single-threaded Lely event loop. */
    std::unique_ptr<lely::ev::Loop> loop;
    /** Executor used to serialize runtime work on loop. */
    std::unique_ptr<lely::ev::Executor> executor;
    /** Monotonic timer service used by CANopen protocols. */
    std::unique_ptr<lely::io::Timer> timer;
    /** SocketCAN controller handle. */
    std::unique_ptr<lely::io::CanController> controller;
    /** Asynchronous CAN channel bound to controller. */
    std::unique_ptr<lely::io::CanChannel> channel;
    /** CANopen master loaded from the configured DCF. */
    std::unique_ptr<lely::canopen::AsyncMaster> master;

    /**
     * @brief Records an internal failure from a noexcept control path.
     *
     * @param operation Operation that failed.
     * @param detail Failure detail or nullptr.
     */
    void ReportNoexceptFailure(const char* operation, const char* detail) noexcept
    {
        try {
            const std::string message = std::string(operation) + ": " + (detail != nullptr ? detail : "unknown error");
            status.SetLastException(message);
            Log(LogLevel::kError, LogCategory::kLely, message);
        } catch (...) {
            std::fprintf(stderr, "[ERROR] [LELY] %s: %s\n", operation,
                         detail != nullptr ? detail : "unknown error");
        }
    }

    /**
     * @brief Prevents exceptions from escaping a noexcept Lely callback.
     *
     * @tparam Function Callback body type.
     * @param operation Callback name used in diagnostics.
     * @param function Callback body.
     */
    template <typename Function>
    void GuardCallback(const char* operation, Function&& function) noexcept
    {
        try {
            function();
        } catch (const std::exception& exception) {
            ReportNoexceptFailure(operation, exception.what());
            StopLoopNoexcept();
        } catch (...) {
            ReportNoexceptFailure(operation, "unknown exception");
            StopLoopNoexcept();
        }
    }

    /** @brief Stops the event loop without allowing an exception to escape. */
    void StopLoopNoexcept() noexcept
    {
        try {
            if (loop) {
                loop->stop();
            }
        } catch (const std::exception& exception) {
            ReportNoexceptFailure("event-loop stop failed", exception.what());
        } catch (...) {
            ReportNoexceptFailure("event-loop stop failed", "unknown exception");
        }
    }

    /**
     * @brief Releases all constructed Lely resources while control_mutex is held.
     *
     * The function is used by both normal shutdown and Start() rollback. It
     * preserves RuntimeStatus so the failure that triggered rollback remains
     * visible to the caller.
     */
    void CleanupResourcesLocked() noexcept
    {
        try {
            if (context) {
                context->shutdown();
            }
        } catch (const std::exception& exception) {
            ReportNoexceptFailure("context shutdown during cleanup failed", exception.what());
        } catch (...) {
            ReportNoexceptFailure("context shutdown during cleanup failed", "unknown exception");
        }

        // Release resources in strict reverse dependency order. This method is
        // also the rollback boundary for a partially completed Start().
        master.reset();
        if (channel) {
            std::error_code error;
            channel->close(error);
            if (error) {
                try {
                    Log(LogLevel::kWarning, LogCategory::kSocketCan,
                        "channel close reported: " + error.message());
                } catch (...) {
                    std::fprintf(stderr, "[WARN] [SOCKETCAN] channel close reported an error\n");
                }
            }
        }
        channel.reset();
        controller.reset();
        timer.reset();
        executor.reset();
        loop.reset();
        poll.reset();
        context.reset();
        io_guard.reset();

        started = false;
        running = false;
        stop_requested = false;
        stop_posted = false;
        run_thread_id = std::thread::id();
    }

    /** @brief Installs runtime status callbacks on AsyncMaster. */
    void ConfigureCallbacks()
    {
        master->OnCanState([this](lely::io::CanState new_state, lely::io::CanState old_state) noexcept {
            GuardCallback("CAN state callback failed", [this, new_state, old_state]() {
                const RuntimeCanState converted = ConvertCanState(new_state);
                std::ostringstream message;
                message << "CAN state changed from " << ToString(ConvertCanState(old_state)) << " to "
                        << ToString(converted);
                const std::string text = message.str();
                status.SetCanStateEvent(converted, text);
                Log(LogLevel::kInfo, LogCategory::kSocketCan, text);
            });
        });

        master->OnCanError([this](lely::io::CanError error) noexcept {
            GuardCallback("CAN error callback failed", [this, error]() {
                std::ostringstream message;
                message << "CAN error flags=0x" << std::hex << static_cast<int>(error);
                const std::string text = message.str();
                status.RecordCanError(text);
                Log(LogLevel::kWarning, LogCategory::kSocketCan, text);
            });
        });

        master->OnHeartbeat([this](std::uint8_t node_id, bool occurred) noexcept {
            GuardCallback("heartbeat callback failed", [this, node_id, occurred]() {
                std::ostringstream message;
                message << "heartbeat " << (occurred ? "occurred" : "recovered") << " for node "
                        << static_cast<unsigned int>(node_id);
                const std::string text = message.str();
                status.SetLastEvent(text);
                Log(occurred ? LogLevel::kWarning : LogLevel::kInfo, LogCategory::kCanOpen, text);
            });
        });

        master->OnState([this](std::uint8_t node_id, lely::canopen::NmtState state) noexcept {
            GuardCallback("NMT state callback failed", [this, node_id, state]() {
                std::ostringstream message;
                message << "NMT state node=" << static_cast<unsigned int>(node_id)
                        << " state=0x" << std::hex << static_cast<int>(state);
                const std::string text = message.str();
                status.SetLastEvent(text);
                Log(LogLevel::kInfo, LogCategory::kCanOpen, text);
            });
        });

        master->OnBoot([this](std::uint8_t node_id, lely::canopen::NmtState state, char boot_status,
                              const std::string& diagnostic) noexcept {
            GuardCallback("boot callback failed", [this, node_id, state, boot_status, diagnostic]() {
                std::ostringstream message;
                message << "boot node=" << static_cast<unsigned int>(node_id)
                        << " state=0x" << std::hex << static_cast<int>(state);
                if (boot_status == 0) {
                    message << " status=success";
                } else {
                    message << " status=" << boot_status;
                }
                if (!diagnostic.empty()) {
                    message << " diagnostic=" << diagnostic;
                }
                const std::string text = message.str();
                status.SetLastEvent(text);
                Log(boot_status == 0 ? LogLevel::kInfo : LogLevel::kWarning,
                    LogCategory::kCanOpen, text);
            });
        });
    }

    /** @brief Starts orderly shutdown on the Lely event-loop thread. */
    void BeginShutdownOnLoop() noexcept
    {
        try {
            status.SetLifecycle(RuntimeLifecycle::kStopping);
            Log(LogLevel::kInfo, LogCategory::kRuntime, "event-loop shutdown started");
            if (context) {
                context->shutdown();
            }
        } catch (const std::exception& exception) {
            ReportNoexceptFailure("event-loop shutdown failed", exception.what());
            StopLoopNoexcept();
        } catch (...) {
            ReportNoexceptFailure("event-loop shutdown failed", "unknown exception");
            StopLoopNoexcept();
        }
    }

    /**
     * @brief Posts at most one shutdown task while control_mutex is held.
     */
    void PostStopRequestLocked() noexcept
    {
        if (!executor || stop_posted) {
            return;
        }
        stop_posted = true;
        try {
            executor->post([this]() noexcept { BeginShutdownOnLoop(); });
        } catch (const std::exception& exception) {
            ReportNoexceptFailure("failed to post shutdown", exception.what());
            StopLoopNoexcept();
        } catch (...) {
            ReportNoexceptFailure("failed to post shutdown", "unknown exception");
            StopLoopNoexcept();
        }
    }
};

LelyRuntime::LelyRuntime(RuntimeStatus& status)
    : impl_(new Impl(status))
{
}

LelyRuntime::~LelyRuntime() noexcept
{
    Stop();
}

void LelyRuntime::Start(const AppConfig& config)
{
    std::lock_guard<std::mutex> lock(impl_->control_mutex);
    if (impl_->started) {
        throw LelyRuntimeError("runtime is already started");
    }

    // Reset the complete per-session status before constructing any Lely
    // resource. This prevents a failure recorded by a previous run from being
    // interpreted as an asynchronous failure in the new Run() call.
    impl_->status.ResetForStart();
    impl_->config = config;
    impl_->running = false;
    impl_->stop_posted = false;
    impl_->run_thread_id = std::thread::id();

    // Start() is transactional. Any exception before Commit() releases every
    // partially constructed object in reverse dependency order while the
    // lifecycle mutex remains held.
    struct StartRollback final {
        explicit StartRollback(Impl& implementation) noexcept
            : impl(implementation)
        {
        }

        ~StartRollback() noexcept
        {
            if (!committed) {
                impl.CleanupResourcesLocked();
            }
        }

        void Commit() noexcept { committed = true; }

        Impl& impl;
        bool committed{false};
    } rollback(*impl_);

    try {
        impl_->io_guard.reset(new lely::io::IoGuard());
        impl_->context.reset(new lely::io::Context());
        impl_->poll.reset(new lely::io::Poll(*impl_->context));
        impl_->loop.reset(new lely::ev::Loop(impl_->poll->get_poll()));
        impl_->executor.reset(new lely::ev::Executor(impl_->loop->get_executor()));
        impl_->timer.reset(new lely::io::Timer(*impl_->poll, *impl_->executor, CLOCK_MONOTONIC));
    } catch (const std::exception& exception) {
        impl_->status.SetLastException(exception.what());
        throw LelyRuntimeError(SystemErrorMessage("Lely I/O initialization", exception));
    }

    try {
        impl_->controller.reset(new lely::io::CanController(config.can_interface.c_str()));

        int nominal_bitrate = 0;
        int data_bitrate = 0;
        impl_->controller->get_bitrate(&nominal_bitrate, &data_bitrate);
        if (nominal_bitrate != static_cast<int>(config.can_bitrate)) {
            std::ostringstream message;
            message << "interface " << config.can_interface << " bitrate is " << nominal_bitrate
                    << " bit/s, expected " << config.can_bitrate << " bit/s";
            throw SocketCanError(message.str());
        }

        const lely::io::CanState initial_state = impl_->controller->get_state();
        const RuntimeCanState runtime_state = ConvertCanState(initial_state);
        impl_->status.SetCanStateEvent(runtime_state,
                                       "initial CAN state=" + std::string(ToString(runtime_state)));
        if (initial_state == lely::io::CanState::STOPPED || initial_state == lely::io::CanState::BUSOFF) {
            throw SocketCanError("interface " + config.can_interface + " is in state "
                                 + ToString(ConvertCanState(initial_state)));
        }

        impl_->channel.reset(new lely::io::CanChannel(*impl_->poll, *impl_->executor, 256, true));
        impl_->channel->open(*impl_->controller);
    } catch (const SocketCanError& exception) {
        impl_->status.SetLastException(exception.what());
        throw;
    } catch (const std::exception& exception) {
        impl_->status.SetLastException(exception.what());
        throw SocketCanError(SystemErrorMessage("SocketCAN initialization for " + config.can_interface, exception));
    }

    try {
        impl_->master.reset(new lely::canopen::AsyncMaster(*impl_->executor, *impl_->timer, *impl_->channel,
                                                           config.master_dcf_path, "", config.master_node_id));
        impl_->master->SetTimeout(std::chrono::milliseconds(config.sdo_timeout_ms));
        impl_->ConfigureCallbacks();
        impl_->master->Reset();
    } catch (const std::exception& exception) {
        impl_->status.SetLastException(exception.what());
        throw LelyRuntimeError(SystemErrorMessage("CANopen master initialization", exception));
    }

    impl_->started = true;
    impl_->status.SetLifecycle(RuntimeLifecycle::kReady);
    Log(LogLevel::kInfo, LogCategory::kRuntime, "Lely runtime is ready");

    if (impl_->stop_requested) {
        impl_->PostStopRequestLocked();
    }
    rollback.Commit();
}

int LelyRuntime::Run()
{
    // Hold control_mutex only long enough to validate and publish run state.
    // Loop::run() must execute without the mutex so RequestStop() and Stop()
    // can make progress from other threads.
    lely::ev::Loop* loop = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->control_mutex);
        if (!impl_->started || !impl_->loop) {
            throw LelyRuntimeError("runtime has not been started");
        }
        if (impl_->running) {
            throw LelyRuntimeError("event loop is already running");
        }
        impl_->status.SetLifecycle(RuntimeLifecycle::kRunning);
        impl_->running = true;
        impl_->run_thread_id = std::this_thread::get_id();
        loop = impl_->loop.get();
    }

    std::error_code error;
    std::size_t executed_tasks = 0;
    std::exception_ptr run_exception;
    try {
        Log(LogLevel::kInfo, LogCategory::kRuntime, "event loop started");
        executed_tasks = loop->run(error);
    } catch (...) {
        run_exception = std::current_exception();
    }

    bool stop_requested = false;
    {
        std::lock_guard<std::mutex> lock(impl_->control_mutex);
        stop_requested = impl_->stop_requested;
    }

    // Delay rethrowing until running is cleared and waiting cleanup threads are
    // notified; otherwise Stop() could remain blocked after a loop failure.
    std::exception_ptr completion_exception;
    try {
        if (run_exception) {
            try {
                std::rethrow_exception(run_exception);
            } catch (const std::exception& exception) {
                impl_->status.SetLastException(exception.what());
                throw LelyRuntimeError(SystemErrorMessage("event loop", exception));
            } catch (...) {
                impl_->status.SetLastException("unknown exception from event loop");
                throw LelyRuntimeError("event loop failed with an unknown exception");
            }
        }
        if (error) {
            impl_->status.SetLastException(error.message());
            throw LelyRuntimeError("event loop failed after " + std::to_string(executed_tasks)
                                   + " tasks: " + error.message());
        }
        const RuntimeStatusSnapshot snapshot = impl_->status.Snapshot();
        if (!snapshot.last_exception.empty()) {
            throw LelyRuntimeError("runtime recorded an asynchronous failure: " + snapshot.last_exception);
        }
        if (!stop_requested) {
            impl_->status.SetLastException("event loop stopped without a stop request");
            throw LelyRuntimeError("event loop stopped unexpectedly after " + std::to_string(executed_tasks)
                                   + " tasks");
        }
        impl_->status.SetLifecycle(RuntimeLifecycle::kStopped);
        Log(LogLevel::kInfo, LogCategory::kRuntime,
            "event loop stopped after " + std::to_string(executed_tasks) + " tasks");
    } catch (...) {
        completion_exception = std::current_exception();
    }

    {
        std::lock_guard<std::mutex> lock(impl_->control_mutex);
        impl_->running = false;
        impl_->run_thread_id = std::thread::id();
    }
    impl_->control_condition.notify_all();

    if (completion_exception) {
        std::rethrow_exception(completion_exception);
    }
    return 0;
}

void LelyRuntime::RequestStop() noexcept
{
    try {
        std::lock_guard<std::mutex> lock(impl_->control_mutex);
        if (impl_->stop_requested) {
            return;
        }
        impl_->stop_requested = true;
        Log(LogLevel::kInfo, LogCategory::kRuntime, "stop requested");
        impl_->PostStopRequestLocked();
    } catch (const std::exception& exception) {
        impl_->ReportNoexceptFailure("stop request failed", exception.what());
        impl_->StopLoopNoexcept();
    } catch (...) {
        impl_->ReportNoexceptFailure("stop request failed", "unknown exception");
        impl_->StopLoopNoexcept();
    }
}

void LelyRuntime::Stop() noexcept
{
    try {
        std::unique_lock<std::mutex> lock(impl_->control_mutex);
        if (!impl_->started && !impl_->io_guard) {
            return;
        }

        if (impl_->running && impl_->loop) {
            impl_->stop_requested = true;
            try {
                Log(LogLevel::kWarning, LogCategory::kRuntime,
                    "forcing event-loop stop during resource cleanup");
            } catch (...) {
                std::fprintf(stderr, "[WARN] [RUNTIME] forcing event-loop stop during resource cleanup\n");
            }
            impl_->StopLoopNoexcept();
            if (impl_->run_thread_id == std::this_thread::get_id()) {
                return;
            }
            impl_->control_condition.wait(lock, [this]() { return !impl_->running; });
        }

        impl_->CleanupResourcesLocked();
        try {
            if (impl_->status.Snapshot().lifecycle != RuntimeLifecycle::kFailed) {
                impl_->status.SetLifecycle(RuntimeLifecycle::kStopped);
            }
            Log(LogLevel::kInfo, LogCategory::kRuntime, "runtime resources released");
        } catch (...) {
            std::fprintf(stderr, "[WARN] [RUNTIME] resources released, but final status logging failed\n");
        }
    } catch (const std::exception& exception) {
        impl_->ReportNoexceptFailure("runtime cleanup failed", exception.what());
    } catch (...) {
        impl_->ReportNoexceptFailure("runtime cleanup failed", "unknown exception");
    }
}

} // namespace canopen_test
