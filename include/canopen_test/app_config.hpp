/**
 * @file
 * @brief Declares validated runtime and asynchronous logging configuration.
 */

#ifndef CANOPEN_TEST_APP_CONFIG_HPP_
#define CANOPEN_TEST_APP_CONFIG_HPP_

#include "canopen_test/logger.hpp"

#include <cstdint>
#include <string>

namespace canopen_test {

/** @brief Validated runtime configuration snapshot loaded from tester.conf. */
struct AppConfig {
    /** Configuration file path supplied to Load(). */
    std::string source_path;
    /** SocketCAN interface name, for example can1. */
    std::string can_interface;
    /** Required nominal CAN bitrate in bit/s. */
    std::uint32_t can_bitrate{0};
    /** CANopen master Node-ID in the range 1..127. */
    std::uint8_t master_node_id{0};
    /** Master DCF path resolved relative to the configuration directory. */
    std::string master_dcf_path;
    /** MCU slave Node-ID in the range 1..127. */
    std::uint8_t slave_node_id{0};
    /** Slave EDS path resolved relative to the configuration directory. */
    std::string slave_eds_path;
    /** Boot-up wait timeout in milliseconds. */
    std::uint32_t boot_timeout_ms{0};
    /** SDO request timeout in milliseconds. */
    std::uint32_t sdo_timeout_ms{0};
    /** NMT state wait timeout in milliseconds. */
    std::uint32_t nmt_state_timeout_ms{0};
    /** PDO event wait timeout in milliseconds. */
    std::uint32_t pdo_timeout_ms{0};
    /** LSS operation timeout in milliseconds. */
    std::uint32_t lss_timeout_ms{0};
    /** Console history path resolved relative to the configuration directory. */
    std::string history_path;
    /** Report directory resolved relative to the configuration directory. */
    std::string report_directory;
    /** Validated asynchronous logging configuration. */
    LoggingConfig logging;

    /**
     * @brief Loads and validates an INI configuration file.
     *
     * Relative paths are resolved against the directory containing the
     * configuration file. Referenced DCF and EDS files must be non-empty.
     *
     * @param path Configuration file path.
     * @return Fully validated runtime configuration.
     * @throws ConfigError if parsing or validation fails.
     */
    static AppConfig Load(const std::string& path);

    /**
     * @brief Returns a redaction-free configuration snapshot for startup logs.
     * @return Multi-line configuration snapshot.
     */
    std::string ToString() const;
};

} // namespace canopen_test

#endif /* CANOPEN_TEST_APP_CONFIG_HPP_ */
