/**
 * @file
 * @brief Declares stable exit codes and typed application errors.
 */

#ifndef CANOPEN_TEST_ERROR_HPP_
#define CANOPEN_TEST_ERROR_HPP_

#include <stdexcept>
#include <string>
#include <utility>

namespace canopen_test {

/**
 * @brief Process exit codes used by the stage-1 executable.
 */
enum class ExitCode : int {
    kSuccess = 0,
    kUsage = 1,
    kConfiguration = 2,
    kSocketCan = 3,
    kLelyRuntime = 4,
    kInternal = 5,
};

/**
 * @brief Base exception carrying a stable process exit code and category.
 */
class ApplicationError : public std::runtime_error {
public:
    /**
     * @brief Constructs an application error.
     *
     * @param code Process exit code associated with the failure.
     * @param category Stable diagnostic category printed to the user.
     * @param message Detailed failure message.
     */
    ApplicationError(ExitCode code, std::string category, const std::string& message)
        : std::runtime_error(message), code_(code), category_(std::move(category))
    {
    }

    /**
     * @brief Returns the process exit code associated with this error.
     *
     * @return Stable process exit code.
     */
    ExitCode code() const noexcept
    {
        return code_;
    }

    /**
     * @brief Returns the diagnostic category associated with this error.
     *
     * @return Diagnostic category string.
     */
    const std::string& category() const noexcept
    {
        return category_;
    }

private:
    ExitCode code_;
    std::string category_;
};

/**
 * @brief Configuration parsing or validation failure.
 */
class ConfigError final : public ApplicationError {
public:
    /**
     * @brief Constructs a configuration error.
     *
     * @param message Detailed failure message.
     */
    explicit ConfigError(const std::string& message)
        : ApplicationError(ExitCode::kConfiguration, "CONFIG", message)
    {
    }
};

/**
 * @brief SocketCAN controller or channel failure.
 */
class SocketCanError final : public ApplicationError {
public:
    /**
     * @brief Constructs a SocketCAN error.
     *
     * @param message Detailed failure message.
     */
    explicit SocketCanError(const std::string& message)
        : ApplicationError(ExitCode::kSocketCan, "SOCKETCAN", message)
    {
    }
};

/**
 * @brief Lely event-loop, DCF, or CANopen runtime failure.
 */
class LelyRuntimeError final : public ApplicationError {
public:
    /**
     * @brief Constructs a Lely runtime error.
     *
     * @param message Detailed failure message.
     */
    explicit LelyRuntimeError(const std::string& message)
        : ApplicationError(ExitCode::kLelyRuntime, "LELY", message)
    {
    }
};

} // namespace canopen_test

#endif /* CANOPEN_TEST_ERROR_HPP_ */
