/**
 * @file
 * @brief Implements bounded non-blocking asynchronous logging with vendored spdlog.
 */

#include "canopen_test/logger.hpp"

#include "canopen_test/error.hpp"

#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <array>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace canopen_test {
namespace {

constexpr std::size_t kCategoryCount = 7U;

/** @brief Process-wide logging objects initialized before worker threads start. */
struct LoggingState {
    std::mutex mutex;
    std::shared_ptr<spdlog::details::thread_pool> thread_pool;
    std::array<std::shared_ptr<spdlog::logger>, kCategoryCount> loggers;
    bool initialized{false};
};

LoggingState& State()
{
    static LoggingState state;
    return state;
}

std::size_t CategoryIndex(LogCategory category) noexcept
{
    switch (category) {
    case LogCategory::kApplication:
        return 0U;
    case LogCategory::kConfiguration:
        return 1U;
    case LogCategory::kRuntime:
        return 2U;
    case LogCategory::kSocketCan:
        return 3U;
    case LogCategory::kCanOpen:
        return 4U;
    case LogCategory::kSignal:
        return 5U;
    case LogCategory::kLely:
        return 6U;
    }
    return 0U;
}

spdlog::level::level_enum ToSpdlogLevel(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::kTrace:
        return spdlog::level::trace;
    case LogLevel::kDebug:
        return spdlog::level::debug;
    case LogLevel::kInfo:
        return spdlog::level::info;
    case LogLevel::kWarning:
        return spdlog::level::warn;
    case LogLevel::kError:
        return spdlog::level::err;
    case LogLevel::kCritical:
        return spdlog::level::critical;
    case LogLevel::kOff:
        return spdlog::level::off;
    }
    return spdlog::level::off;
}

LogLevel CategoryLevel(const LoggingConfig& config, LogCategory category) noexcept
{
    switch (category) {
    case LogCategory::kApplication:
        return config.application_level;
    case LogCategory::kConfiguration:
        return config.configuration_level;
    case LogCategory::kRuntime:
        return config.runtime_level;
    case LogCategory::kSocketCan:
        return config.socketcan_level;
    case LogCategory::kCanOpen:
        return config.canopen_level;
    case LogCategory::kSignal:
        return config.signal_level;
    case LogCategory::kLely:
        return config.lely_level;
    }
    return LogLevel::kOff;
}

bool IsFallbackLevel(LogLevel level) noexcept
{
    return level == LogLevel::kWarning || level == LogLevel::kError || level == LogLevel::kCritical;
}

void FallbackWrite(LogLevel level, LogCategory category, const std::string& message) noexcept
{
    if (!IsFallbackLevel(level)) {
        return;
    }
    std::fprintf(stderr, "[%s] [%s] %s\n", ToString(level), ToString(category), message.c_str());
}

std::shared_ptr<spdlog::logger> FindLogger(LogCategory category) noexcept
{
    try {
        LoggingState& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.initialized) {
            return {};
        }
        return state.loggers[CategoryIndex(category)];
    } catch (...) {
        return {};
    }
}

} // namespace

void InitializeLogging(const LoggingConfig& config)
{
    LoggingState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.initialized) {
        throw ConfigError("logging backend is already initialized");
    }
    if (config.async_queue_size == 0U || config.async_worker_count == 0U) {
        throw ConfigError("logging queue size and worker count must be greater than zero");
    }

    try {
        auto thread_pool = std::make_shared<spdlog::details::thread_pool>(
            config.async_queue_size, config.async_worker_count);

        std::vector<spdlog::sink_ptr> sinks;
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(ToSpdlogLevel(config.console_level));
        console_sink->set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] [%n] %v");
        sinks.push_back(console_sink);

        if (config.file_enabled) {
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                config.file_path, config.file_max_size, config.file_count, false);
            file_sink->set_level(ToSpdlogLevel(config.file_level));
            file_sink->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] [%n] [tid=%t] %v");
            sinks.push_back(file_sink);
        }

        std::array<std::shared_ptr<spdlog::logger>, kCategoryCount> loggers;
        const std::array<LogCategory, kCategoryCount> categories {{
            LogCategory::kApplication,
            LogCategory::kConfiguration,
            LogCategory::kRuntime,
            LogCategory::kSocketCan,
            LogCategory::kCanOpen,
            LogCategory::kSignal,
            LogCategory::kLely,
        }};

        for (const LogCategory category : categories) {
            auto logger = std::make_shared<spdlog::async_logger>(
                ToString(category), sinks.begin(), sinks.end(), thread_pool,
                spdlog::async_overflow_policy::overrun_oldest);
            logger->set_level(ToSpdlogLevel(CategoryLevel(config, category)));
            logger->flush_on(ToSpdlogLevel(config.flush_level));
            loggers[CategoryIndex(category)] = std::move(logger);
        }

        state.thread_pool = std::move(thread_pool);
        state.loggers = std::move(loggers);
        state.initialized = true;
    } catch (const std::exception& exception) {
        throw ConfigError("failed to initialize asynchronous logging: " + std::string(exception.what()));
    }
}

bool ShouldLog(LogLevel level, LogCategory category) noexcept
{
    const std::shared_ptr<spdlog::logger> logger = FindLogger(category);
    return logger && logger->should_log(ToSpdlogLevel(level));
}

void Log(LogLevel level, LogCategory category, const std::string& message) noexcept
{
    try {
        const std::shared_ptr<spdlog::logger> logger = FindLogger(category);
        if (!logger) {
            FallbackWrite(level, category, message);
            return;
        }
        logger->log(ToSpdlogLevel(level), message);
    } catch (...) {
        FallbackWrite(level, category, message);
    }
}

void ShutdownLogging() noexcept
{
    std::array<std::shared_ptr<spdlog::logger>, kCategoryCount> loggers;
    std::shared_ptr<spdlog::details::thread_pool> thread_pool;
    try {
        LoggingState& state = State();
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (!state.initialized) {
                return;
            }
            state.initialized = false;
            loggers = std::move(state.loggers);
            thread_pool = std::move(state.thread_pool);
        }

        for (const auto& logger : loggers) {
            if (logger) {
                logger->flush();
            }
        }
        loggers = {};
        thread_pool.reset();
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "[ERROR] [application] logging shutdown failed: %s\n", exception.what());
    } catch (...) {
        std::fprintf(stderr, "[ERROR] [application] logging shutdown failed with an unknown exception\n");
    }
}

std::uint64_t DroppedLogCount() noexcept
{
    try {
        LoggingState& state = State();
        std::shared_ptr<spdlog::details::thread_pool> thread_pool;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            thread_pool = state.thread_pool;
        }
        return thread_pool ? static_cast<std::uint64_t>(thread_pool->overrun_counter()) : 0U;
    } catch (...) {
        return 0U;
    }
}

const char* ToString(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::kTrace:
        return "trace";
    case LogLevel::kDebug:
        return "debug";
    case LogLevel::kInfo:
        return "info";
    case LogLevel::kWarning:
        return "warning";
    case LogLevel::kError:
        return "error";
    case LogLevel::kCritical:
        return "critical";
    case LogLevel::kOff:
        return "off";
    }
    return "off";
}

const char* ToString(LogCategory category) noexcept
{
    switch (category) {
    case LogCategory::kApplication:
        return "application";
    case LogCategory::kConfiguration:
        return "configuration";
    case LogCategory::kRuntime:
        return "runtime";
    case LogCategory::kSocketCan:
        return "socketcan";
    case LogCategory::kCanOpen:
        return "canopen";
    case LogCategory::kSignal:
        return "signal";
    case LogCategory::kLely:
        return "lely";
    }
    return "application";
}

} // namespace canopen_test
