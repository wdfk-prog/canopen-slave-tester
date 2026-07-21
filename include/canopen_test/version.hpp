/**
 * @file
 * @brief Declares build-time software and dependency version accessors.
 */

#ifndef CANOPEN_TEST_VERSION_HPP_
#define CANOPEN_TEST_VERSION_HPP_

#include "canopen_test/version_config.hpp"

namespace canopen_test {

/**
 * @brief Returns the tester semantic version.
 * @return Static semantic version string.
 */
inline const char* TesterVersion() noexcept
{
    return CANOPEN_TESTER_VERSION;
}

/**
 * @brief Returns the Lely version selected from staged package metadata.
 * @return Static build dependency version string.
 */
inline const char* BuiltAgainstLelyVersion() noexcept
{
    return CANOPEN_TESTER_LELY_VERSION;
}

/**
 * @brief Returns the vendored spdlog header version.
 * @return Static spdlog version string.
 */
inline const char* SpdlogVersion() noexcept
{
    return CANOPEN_TESTER_SPDLOG_VERSION;
}

} // namespace canopen_test

#endif /* CANOPEN_TEST_VERSION_HPP_ */
