/**
 * @file
 * @brief Implements the self-pipe based POSIX signal bridge.
 */

#include "canopen_test/signal_handler.hpp"

#include "canopen_test/logger.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace canopen_test {
namespace {

/**
 * Write descriptor visible to the async signal trampoline. Only one handler
 * instance is supported because POSIX signal actions are process-global.
 */
volatile sig_atomic_t g_signal_write_fd = -1;

/**
 * @brief Closes a valid descriptor and resets it to the invalid sentinel.
 *
 * @param descriptor Descriptor storage to close and reset.
 */
void CloseFileDescriptor(int& descriptor) noexcept
{
    if (descriptor >= 0) {
        close(descriptor);
        descriptor = -1;
    }
}

/**
 * @brief Enables non-blocking writes for the signal-side pipe descriptor.
 *
 * @param descriptor Descriptor to update.
 * @throws std::system_error if fcntl() fails.
 */
void SetNonBlocking(int descriptor)
{
    const int flags = fcntl(descriptor, F_GETFL, 0);
    if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::system_error(errno, std::generic_category(), "fcntl(O_NONBLOCK)");
    }
}

/**
 * @brief Prevents a self-pipe descriptor from leaking across exec().
 *
 * @param descriptor Descriptor to update.
 * @throws std::system_error if fcntl() fails.
 */
void SetCloseOnExec(int descriptor)
{
    const int flags = fcntl(descriptor, F_GETFD, 0);
    if (flags < 0 || fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0) {
        throw std::system_error(errno, std::generic_category(), "fcntl(FD_CLOEXEC)");
    }
}

/**
 * @brief Logs listener-thread failures without propagating exceptions.
 *
 * @param level Log severity.
 * @param message Primary message.
 * @param detail Optional appended detail.
 */
void SignalLogNoexcept(LogLevel level, const char* message, const char* detail = nullptr) noexcept
{
    try {
        std::string text(message);
        if (detail != nullptr) {
            text += detail;
        }
        Log(level, LogCategory::kSignal, text);
    } catch (...) {
        std::fprintf(stderr, "[%s] [SIGNAL] %s%s\n", level == LogLevel::kError ? "ERROR" : "WARN", message,
                     detail != nullptr ? detail : "");
    }
}

} // namespace

struct SignalHandler::ListenerState {
    ListenerState(Callback listener_callback, int listener_read_fd)
        : callback(std::move(listener_callback))
        , read_fd(listener_read_fd)
    {
    }

    ~ListenerState() noexcept
    {
        CloseFileDescriptor(read_fd);
    }

    Callback callback;
    int read_fd{-1};
};

SignalHandler::SignalHandler(Callback callback)
{
    if (!callback) {
        throw std::invalid_argument("SignalHandler callback is empty");
    }
    if (g_signal_write_fd >= 0) {
        throw std::logic_error("only one SignalHandler instance is supported");
    }

    // The self-pipe transfers only the signal number from async signal context
    // to the listener thread, where normal C++ APIs and logging are safe.
    int descriptors[2] {-1, -1};
    if (pipe(descriptors) != 0) {
        throw std::system_error(errno, std::generic_category(), "pipe");
    }

    write_fd_ = descriptors[1];
    try {
        SetCloseOnExec(descriptors[0]);
        SetCloseOnExec(write_fd_);
        SetNonBlocking(write_fd_);
        listener_state_ = std::make_shared<ListenerState>(std::move(callback), descriptors[0]);
        descriptors[0] = -1;

        struct sigaction action {};
        sigemptyset(&action.sa_mask);
        action.sa_handler = &SignalHandler::HandleSignal;
        action.sa_flags = 0;

        g_signal_write_fd = write_fd_;
        if (sigaction(SIGINT, &action, &old_sigint_) != 0) {
            throw std::system_error(errno, std::generic_category(), "sigaction(SIGINT)");
        }
        sigint_installed_ = true;
        if (sigaction(SIGTERM, &action, &old_sigterm_) != 0) {
            throw std::system_error(errno, std::generic_category(), "sigaction(SIGTERM)");
        }
        sigterm_installed_ = true;
        thread_ = std::thread(&SignalHandler::Listen, listener_state_);
    } catch (...) {
        g_signal_write_fd = -1;
        if (sigterm_installed_) {
            sigaction(SIGTERM, &old_sigterm_, nullptr);
        }
        if (sigint_installed_) {
            sigaction(SIGINT, &old_sigint_, nullptr);
        }
        CloseFileDescriptor(descriptors[0]);
        CloseFileDescriptor(write_fd_);
        listener_state_.reset();
        throw;
    }
}

SignalHandler::~SignalHandler() noexcept
{
    g_signal_write_fd = -1;
    if (sigterm_installed_) {
        sigaction(SIGTERM, &old_sigterm_, nullptr);
    }
    if (sigint_installed_) {
        sigaction(SIGINT, &old_sigint_, nullptr);
    }

    CloseFileDescriptor(write_fd_);
    if (thread_.joinable()) {
        try {
            if (thread_.get_id() == std::this_thread::get_id()) {
                thread_.detach();
            } else {
                thread_.join();
            }
        } catch (const std::exception& exception) {
            SignalLogNoexcept(LogLevel::kError, "signal listener shutdown failed: ", exception.what());
            try {
                if (thread_.joinable()) {
                    thread_.detach();
                }
            } catch (...) {
                std::fprintf(stderr, "[ERROR] [SIGNAL] signal listener detach failed\n");
                std::terminate();
            }
        }
    }
    listener_state_.reset();
}

void SignalHandler::HandleSignal(int signal_number) noexcept
{
    // Preserve errno because asynchronous signal delivery must not perturb the
    // interrupted code path. A full pipe intentionally drops the notification.
    const int saved_errno = errno;
    const int descriptor = static_cast<int>(g_signal_write_fd);
    if (descriptor >= 0) {
        const ssize_t result = write(descriptor, &signal_number, sizeof(signal_number));
        static_cast<void>(result);
    }
    errno = saved_errno;
}

void SignalHandler::Listen(std::shared_ptr<ListenerState> state) noexcept
{
    // Closing the write descriptor during destruction produces POLLHUP and
    // wakes poll(), allowing the listener thread to terminate without cancellation.
    struct pollfd poll_descriptor {};
    poll_descriptor.fd = state->read_fd;
    poll_descriptor.events = POLLIN | POLLHUP;

    for (;;) {
        const int result = poll(&poll_descriptor, 1, -1);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            SignalLogNoexcept(LogLevel::kError, "poll failed: ", std::strerror(errno));
            return;
        }
        if ((poll_descriptor.revents & POLLHUP) != 0) {
            return;
        }
        if ((poll_descriptor.revents & POLLIN) == 0) {
            continue;
        }

        int signal_number = 0;
        const ssize_t bytes = read(state->read_fd, &signal_number, sizeof(signal_number));
        if (bytes == 0) {
            return;
        }
        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            SignalLogNoexcept(LogLevel::kError, "read failed: ", std::strerror(errno));
            return;
        }
        if (bytes != static_cast<ssize_t>(sizeof(signal_number))) {
            SignalLogNoexcept(LogLevel::kWarning, "discarded a partial signal notification");
            continue;
        }

        try {
            state->callback(signal_number);
        } catch (const std::exception& exception) {
            SignalLogNoexcept(LogLevel::kError, "stop callback failed: ", exception.what());
        } catch (...) {
            SignalLogNoexcept(LogLevel::kError, "stop callback failed with an unknown exception");
        }
    }
}

} // namespace canopen_test
