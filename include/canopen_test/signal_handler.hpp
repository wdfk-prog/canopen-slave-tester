/**
 * @file
 * @brief Declares the POSIX self-pipe signal bridge.
 */

#ifndef CANOPEN_TEST_SIGNAL_HANDLER_HPP_
#define CANOPEN_TEST_SIGNAL_HANDLER_HPP_

#include <csignal>
#include <functional>
#include <memory>
#include <thread>

namespace canopen_test {

/**
 * @brief Converts SIGINT/SIGTERM into a normal-thread stop callback.
 *
 * The POSIX signal trampoline performs only an async-signal-safe write to a
 * non-blocking pipe. The user callback runs on an internal listener thread and
 * may therefore call normal C++ runtime APIs.
 */
class SignalHandler final {
public:
    /**
     * @brief Stop callback signature.
     *
     * @param signal_number Received POSIX signal number.
     */
    using Callback = std::function<void(int signal_number)>;

    /**
     * @brief Installs SIGINT/SIGTERM handling and starts the listener thread.
     *
     * @param callback Callback executed outside signal context.
     *
     * @throws std::invalid_argument if callback is empty.
     * @throws std::logic_error if another SignalHandler instance is active.
     * @throws std::system_error if pipe, descriptor, signal action, or thread setup fails.
     */
    explicit SignalHandler(Callback callback);

    SignalHandler(const SignalHandler&) = delete;
    SignalHandler& operator=(const SignalHandler&) = delete;

    /**
     * @brief Restores prior signal actions and stops the listener thread.
     */
    ~SignalHandler() noexcept;

private:
    struct ListenerState;

    /**
     * @brief Async-signal-safe signal trampoline.
     *
     * @param signal_number Received POSIX signal number.
     */
    static void HandleSignal(int signal_number) noexcept;

    /**
     * @brief Processes pipe events on the listener thread.
     *
     * @param state Shared listener state that remains valid until the thread exits.
     */
    static void Listen(std::shared_ptr<ListenerState> state) noexcept;

    std::shared_ptr<ListenerState> listener_state_;
    int write_fd_{-1};
    struct sigaction old_sigint_ {};
    struct sigaction old_sigterm_ {};
    bool sigint_installed_{false};
    bool sigterm_installed_{false};
    std::thread thread_;
};

} // namespace canopen_test

#endif /* CANOPEN_TEST_SIGNAL_HANDLER_HPP_ */
