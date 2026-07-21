/**
 * @file
 * @brief Implements command-line processing and runtime orchestration.
 */

#include "canopen_test/application.hpp"

#include "canopen_test/app_config.hpp"
#include "canopen_test/error.hpp"
#include "canopen_test/lely_runtime.hpp"
#include "canopen_test/logger.hpp"
#include "canopen_test/runtime_status.hpp"
#include "canopen_test/signal_handler.hpp"
#include "canopen_test/version.hpp"

#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace canopen_test {
namespace {

constexpr const char* kDefaultConfigPath = "/opt/Ultra/Debug/canopen-slave-tester/config/tester.conf";

/** @brief Parsed command-line options used by Application::Run(). */
struct CommandLineOptions {
    std::string config_path{kDefaultConfigPath};  /**< Runtime configuration file path. */
    bool check_config{false};                     /**< Validate configuration and exit. */
    bool show_help{false};                        /**< Print command-line help and exit. */
    bool show_version{false};                     /**< Print build versions and exit. */
};

/** @brief Ensures asynchronous logging is stopped on every exit path. */
class LoggingSession final {
public:
    explicit LoggingSession(const LoggingConfig& config)
    {
        InitializeLogging(config);
    }

    LoggingSession(const LoggingSession&) = delete;
    LoggingSession& operator=(const LoggingSession&) = delete;

    ~LoggingSession() noexcept
    {
        ShutdownLogging();
    }
};

CommandLineOptions ParseArguments(int argc, char* argv[])
{
    CommandLineOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--config") {
            if (++index >= argc) {
                throw ApplicationError(ExitCode::kUsage, "USAGE", "--config requires a path");
            }
            options.config_path = argv[index];
        } else if (argument == "--check-config") {
            options.check_config = true;
        } else if (argument == "--help" || argument == "-h") {
            options.show_help = true;
        } else if (argument == "--version") {
            options.show_version = true;
        } else {
            throw ApplicationError(ExitCode::kUsage, "USAGE", "unknown argument '" + argument + "'");
        }
    }
    return options;
}

void PrintHelp(const char* executable)
{
    std::cout << "Usage: " << executable << " [options]\n\n"
              << "Options:\n"
              << "  --config PATH   Load runtime configuration from PATH.\n"
              << "  --check-config  Validate configuration and referenced files, then exit.\n"
              << "  --version       Print software and build dependency versions.\n"
              << "  -h, --help      Print this help text.\n";
}

void PrintVersion()
{
    std::cout << "canopen_slave_tester=" << TesterVersion() << '\n'
              << "stage=P1-runtime\n"
              << "built_against_lely=" << BuiltAgainstLelyVersion() << '\n'
              << "spdlog=" << SpdlogVersion() << std::endl;
}

std::string SignalName(int signal_number)
{
    switch (signal_number) {
    case SIGINT:
        return "SIGINT";
    case SIGTERM:
        return "SIGTERM";
    default:
        return "signal " + std::to_string(signal_number);
    }
}

/**
 * @brief Records a runtime failure while the logging session is still active.
 *
 * @param exception_type C++ exception class used by the top-level handler.
 * @param exception_category Stable diagnostic category associated with the failure.
 * @param exit_code Stable process exit code associated with the failure.
 * @param detail Failure description.
 * @param status Runtime status recorder used to capture the failure snapshot.
 */
void LogRuntimeFailure(const char* exception_type, const char* exception_category, int exit_code,
                       const char* detail, RuntimeStatus& status) noexcept
{
    try {
        const RuntimeStatusSnapshot snapshot = status.Snapshot();
        std::ostringstream message;
        message << "runtime failure exception_type=" << exception_type
                << " exception_category=" << exception_category << " exit_code=" << exit_code
                << " detail=" << detail << " session=" << snapshot.session_id
                << " lifecycle=" << ToString(snapshot.lifecycle)
                << " can_state=" << ToString(snapshot.can_state)
                << " can_errors=" << snapshot.can_error_count << " dropped_logs=" << DroppedLogCount();
        if (!snapshot.last_event.empty()) {
            message << " last_event=" << snapshot.last_event;
        }
        if (!snapshot.last_exception.empty()) {
            message << " last_exception=" << snapshot.last_exception;
        }
        Log(LogLevel::kError, LogCategory::kRuntime, message.str());
    } catch (...) {
        std::fprintf(stderr,
                     "[ERROR] [RUNTIME] runtime failure exception_type=%s exception_category=%s "
                     "exit_code=%d detail=%s; status snapshot unavailable\n",
                     exception_type, exception_category, exit_code, detail);
    }
}

} // namespace

int Application::Run(int argc, char* argv[])
{
    const CommandLineOptions options = ParseArguments(argc, argv);
    if (options.show_help) {
        PrintHelp(argv[0]);
        return static_cast<int>(ExitCode::kSuccess);
    }
    if (options.show_version) {
        PrintVersion();
        return static_cast<int>(ExitCode::kSuccess);
    }

    const AppConfig config = AppConfig::Load(options.config_path);
    if (options.check_config) {
        std::cout << "configuration: PASS" << std::endl;
        return static_cast<int>(ExitCode::kSuccess);
    }

    LoggingSession logging(config.logging);
    Log(LogLevel::kInfo, LogCategory::kConfiguration, "configuration validated\n" + config.ToString());
    PrintVersion();

    RuntimeStatus status;
    LelyRuntime runtime(status);
    std::unique_ptr<SignalHandler> signals;
    try {
        signals.reset(new SignalHandler([&runtime](int signal_number) {
            Log(LogLevel::kInfo, LogCategory::kSignal, SignalName(signal_number) + " received");
            runtime.RequestStop();
        }));

        runtime.Start(config);
        const int result = runtime.Run();
        signals.reset();
        runtime.Stop();

        const RuntimeStatusSnapshot snapshot = status.Snapshot();
        std::ostringstream message;
        message << "session=" << snapshot.session_id << " lifecycle=" << ToString(snapshot.lifecycle)
                << " can_state=" << ToString(snapshot.can_state) << " can_errors=" << snapshot.can_error_count
                << " dropped_logs=" << DroppedLogCount();
        if (!snapshot.last_event.empty()) {
            message << " last_event=" << snapshot.last_event;
        }
        Log(LogLevel::kInfo, LogCategory::kRuntime, message.str());
        return result;
    } catch (const ApplicationError& exception) {
        LogRuntimeFailure("ApplicationError", exception.category().c_str(), static_cast<int>(exception.code()),
                          exception.what(), status);
        signals.reset();
        runtime.Stop();
        throw;
    } catch (const std::exception& exception) {
        LogRuntimeFailure("std::exception", "INTERNAL", static_cast<int>(ExitCode::kInternal),
                          exception.what(), status);
        signals.reset();
        runtime.Stop();
        throw;
    } catch (...) {
        LogRuntimeFailure("unknown", "INTERNAL", static_cast<int>(ExitCode::kInternal),
                          "unknown exception", status);
        signals.reset();
        runtime.Stop();
        throw;
    }
}

} // namespace canopen_test
