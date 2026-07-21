/**
 * @file
 * @brief Declares the asynchronous process logging interface.
 */

#ifndef CANOPEN_TEST_LOGGER_HPP_
#define CANOPEN_TEST_LOGGER_HPP_

#include <cstddef>
#include <cstdint>
#include <string>

namespace canopen_test {

/** @brief Runtime log severity ordered from most verbose to disabled. */
enum class LogLevel {
    kTrace,
    kDebug,
    kInfo,
    kWarning,
    kError,
    kCritical,
    kOff,
};

/** @brief Stable subsystem identifiers used as spdlog logger names. */
enum class LogCategory {
    kApplication,
    kConfiguration,
    kRuntime,
    kSocketCan,
    kCanOpen,
    kSignal,
    kLely,
};

/** @brief Validated asynchronous logging configuration. */
struct LoggingConfig {
    /** Minimum level accepted by application messages. */
    LogLevel application_level{LogLevel::kInfo};
    /** Minimum level accepted by configuration messages. */
    LogLevel configuration_level{LogLevel::kInfo};
    /** Minimum level accepted by runtime messages. */
    LogLevel runtime_level{LogLevel::kInfo};
    /** Minimum level accepted by SocketCAN messages. */
    LogLevel socketcan_level{LogLevel::kInfo};
    /** Minimum level accepted by CANopen protocol messages. */
    LogLevel canopen_level{LogLevel::kInfo};
    /** Minimum level accepted by signal bridge messages. */
    LogLevel signal_level{LogLevel::kInfo};
    /** Minimum level accepted by Lely integration messages. */
    LogLevel lely_level{LogLevel::kInfo};
    /** Minimum level written to the console sink. */
    LogLevel console_level{LogLevel::kInfo};
    /** Minimum level written to the rotating file sink. */
    LogLevel file_level{LogLevel::kDebug};
    /** Level that requests an asynchronous sink flush. */
    LogLevel flush_level{LogLevel::kError};
    /** Number of preallocated messages in the shared asynchronous queue. */
    std::size_t async_queue_size{4096U};
    /** Number of workers draining the shared asynchronous queue. */
    std::size_t async_worker_count{1U};
    /** Enables the rotating file sink when true. */
    bool file_enabled{true};
    /** File path resolved relative to the configuration directory. */
    std::string file_path;
    /** Maximum size of one active log file before rotation. */
    std::size_t file_max_size{5U * 1024U * 1024U};
    /** Number of rotated files retained in addition to the active file. */
    std::size_t file_count{3U};
};

/**
 * @brief Initializes the process-wide asynchronous logging backend.
 *
 * The backend uses a bounded preallocated queue and the non-blocking
 * overrun-oldest policy so Lely callbacks never wait for console or file I/O.
 *
 * @param config Validated logging configuration.
 * @throws ConfigError if the requested sinks cannot be created.
 */
void InitializeLogging(const LoggingConfig& config);

/**
 * @brief Returns whether a category accepts the specified level.
 *
 * @param level Log severity.
 * @param category Log category.
 * @return true when formatting and submitting the message is useful.
 */
bool ShouldLog(LogLevel level, LogCategory category) noexcept;

/**
 * @brief Submits one message to the asynchronous logging queue.
 *
 * Before initialization or after shutdown, warning and higher messages fall
 * back to stderr. Logging failures never escape to the caller.
 *
 * @param level Log severity.
 * @param category Log category.
 * @param message Diagnostic message.
 */
void Log(LogLevel level, LogCategory category, const std::string& message) noexcept;

/**
 * @brief Flushes queued messages and stops the logging worker threads.
 */
void ShutdownLogging() noexcept;

/**
 * @brief Returns the number of asynchronous messages overwritten since initialization.
 *
 * @return Shared queue overrun count, or zero when logging is not initialized.
 */
std::uint64_t DroppedLogCount() noexcept;

/**
 * @brief Converts a configured log level to its stable lowercase name.
 *
 * @param level Log severity.
 * @return Static lowercase name.
 */
const char* ToString(LogLevel level) noexcept;

/**
 * @brief Converts a log category to its stable lowercase name.
 *
 * @param category Log category.
 * @return Static lowercase name.
 */
const char* ToString(LogCategory category) noexcept;

} // namespace canopen_test

#endif /* CANOPEN_TEST_LOGGER_HPP_ */
