/**
 * @file
 * @brief Implements strict INI parsing and runtime configuration validation.
 */

#include "canopen_test/app_config.hpp"

#include "canopen_test/error.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>

namespace canopen_test {
namespace {

using IniMap = std::map<std::string, std::string>;

std::string Trim(const std::string& input)
{
    const auto first = std::find_if_not(input.begin(), input.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
    if (first == input.end()) {
        return {};
    }

    const auto last = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char character) {
        return std::isspace(character) != 0;
    }).base();
    return std::string(first, last);
}

std::string DirectoryName(const std::string& path)
{
    const auto separator = path.find_last_of('/');
    if (separator == std::string::npos) {
        return ".";
    }
    if (separator == 0U) {
        return "/";
    }
    return path.substr(0U, separator);
}

std::string ResolvePath(const std::string& base_directory, const std::string& path)
{
    if (path.empty() || path.front() == '/') {
        return path;
    }
    if (base_directory == "/") {
        return '/' + path;
    }
    return base_directory + '/' + path;
}

IniMap ParseIni(const std::string& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        throw ConfigError("cannot open configuration file '" + path + "'");
    }

    IniMap values;
    std::string section;
    std::string line;
    std::size_t line_number = 0U;
    while (std::getline(input, line)) {
        ++line_number;
        line = Trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }

        if (line.front() == '[') {
            if (line.back() != ']') {
                throw ConfigError(path + ':' + std::to_string(line_number) + ": malformed section header");
            }
            section = Trim(line.substr(1U, line.size() - 2U));
            if (section.empty()) {
                throw ConfigError(path + ':' + std::to_string(line_number) + ": empty section name");
            }
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            throw ConfigError(path + ':' + std::to_string(line_number) + ": expected key=value");
        }
        if (section.empty()) {
            throw ConfigError(path + ':' + std::to_string(line_number) + ": key outside a section");
        }

        const std::string key = Trim(line.substr(0U, equals));
        const std::string value = Trim(line.substr(equals + 1U));
        if (key.empty()) {
            throw ConfigError(path + ':' + std::to_string(line_number) + ": empty key");
        }

        const std::string qualified_key = section + '.' + key;
        if (!values.emplace(qualified_key, value).second) {
            throw ConfigError(path + ':' + std::to_string(line_number) + ": duplicate key '" + qualified_key + "'");
        }
    }

    if (!input.eof()) {
        throw ConfigError("failed while reading configuration file '" + path + "'");
    }
    return values;
}

void ValidateKnownKeys(const IniMap& values)
{
    static const char* const allowed_keys[] = {
        "can.interface",
        "can.bitrate",
        "master.node_id",
        "master.dcf_path",
        "slave.node_id",
        "slave.eds_path",
        "timeouts.boot_ms",
        "timeouts.sdo_ms",
        "timeouts.nmt_state_ms",
        "timeouts.pdo_ms",
        "timeouts.lss_ms",
        "console.history_path",
        "console.report_directory",
        "logging.application_level",
        "logging.configuration_level",
        "logging.runtime_level",
        "logging.socketcan_level",
        "logging.canopen_level",
        "logging.signal_level",
        "logging.lely_level",
        "logging.console_level",
        "logging.file_level",
        "logging.flush_level",
        "logging.async_queue_size",
        "logging.async_worker_count",
        "logging.overflow_policy",
        "logging.file_enabled",
        "logging.file_path",
        "logging.file_max_size",
        "logging.file_count",
    };

    for (const auto& entry : values) {
        const bool known = std::any_of(std::begin(allowed_keys), std::end(allowed_keys),
                                       [&entry](const char* key) { return entry.first == key; });
        if (!known) {
            throw ConfigError("unsupported configuration key '" + entry.first + "'");
        }
    }
}

const std::string& Required(const IniMap& values, const std::string& key)
{
    const auto entry = values.find(key);
    if (entry == values.end() || entry->second.empty()) {
        throw ConfigError("missing required configuration key '" + key + "'");
    }
    return entry->second;
}

std::string Optional(const IniMap& values, const std::string& key, const std::string& fallback)
{
    const auto entry = values.find(key);
    return entry == values.end() ? fallback : entry->second;
}

std::uint32_t ParseUnsignedText(const std::string& key, const std::string& text,
                                std::uint32_t minimum, std::uint32_t maximum)
{
    if (text.empty() || text.front() == '-') {
        throw ConfigError("configuration key '" + key + "' must be unsigned");
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 0);
    if (errno == ERANGE || end == text.c_str() || *end != '\0'
        || value > std::numeric_limits<std::uint32_t>::max()) {
        throw ConfigError("configuration key '" + key + "' is not a valid unsigned integer");
    }
    if (value < minimum || value > maximum) {
        throw ConfigError("configuration key '" + key + "' must be in range ["
                          + std::to_string(minimum) + ", " + std::to_string(maximum) + "]");
    }
    return static_cast<std::uint32_t>(value);
}

std::uint32_t ParseUnsigned(const IniMap& values, const std::string& key,
                            std::uint32_t minimum, std::uint32_t maximum)
{
    return ParseUnsignedText(key, Required(values, key), minimum, maximum);
}

std::uint32_t ParseOptionalUnsigned(const IniMap& values, const std::string& key,
                                    std::uint32_t fallback, std::uint32_t minimum,
                                    std::uint32_t maximum)
{
    return ParseUnsignedText(key, Optional(values, key, std::to_string(fallback)), minimum, maximum);
}

bool ParseOptionalBool(const IniMap& values, const std::string& key, bool fallback)
{
    const std::string value = Optional(values, key, fallback ? "true" : "false");
    if (value == "true" || value == "1") {
        return true;
    }
    if (value == "false" || value == "0") {
        return false;
    }
    throw ConfigError("configuration key '" + key + "' must be true, false, 1, or 0");
}

LogLevel ParseOptionalLogLevel(const IniMap& values, const std::string& key, LogLevel fallback)
{
    const std::string value = Optional(values, key, ToString(fallback));
    if (value == "trace") {
        return LogLevel::kTrace;
    }
    if (value == "debug") {
        return LogLevel::kDebug;
    }
    if (value == "info") {
        return LogLevel::kInfo;
    }
    if (value == "warning" || value == "warn") {
        return LogLevel::kWarning;
    }
    if (value == "error") {
        return LogLevel::kError;
    }
    if (value == "critical") {
        return LogLevel::kCritical;
    }
    if (value == "off") {
        return LogLevel::kOff;
    }
    throw ConfigError("configuration key '" + key
                      + "' must be trace, debug, info, warning, error, critical, or off");
}

void ValidateCanInterface(const std::string& interface_name)
{
    if (interface_name.empty() || interface_name.size() >= 16U) {
        throw ConfigError("can.interface must contain 1 to 15 characters");
    }
    const bool valid = std::all_of(interface_name.begin(), interface_name.end(), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '_' || character == '-' || character == '.';
    });
    if (!valid) {
        throw ConfigError("can.interface contains unsupported characters");
    }
}

void ValidateRegularFile(const std::string& key, const std::string& path)
{
    struct stat status {};
    if (stat(path.c_str(), &status) != 0) {
        throw ConfigError("configuration key '" + key + "' references unavailable file '" + path + "'");
    }
    if (!S_ISREG(status.st_mode) || status.st_size <= 0) {
        throw ConfigError("configuration key '" + key + "' must reference a non-empty regular file");
    }
}

void LoadLoggingConfig(const IniMap& values, const std::string& base_directory, LoggingConfig& logging)
{
    logging.application_level = ParseOptionalLogLevel(values, "logging.application_level", LogLevel::kInfo);
    logging.configuration_level = ParseOptionalLogLevel(values, "logging.configuration_level", LogLevel::kInfo);
    logging.runtime_level = ParseOptionalLogLevel(values, "logging.runtime_level", LogLevel::kInfo);
    logging.socketcan_level = ParseOptionalLogLevel(values, "logging.socketcan_level", LogLevel::kInfo);
    logging.canopen_level = ParseOptionalLogLevel(values, "logging.canopen_level", LogLevel::kInfo);
    logging.signal_level = ParseOptionalLogLevel(values, "logging.signal_level", LogLevel::kInfo);
    logging.lely_level = ParseOptionalLogLevel(values, "logging.lely_level", LogLevel::kInfo);
    logging.console_level = ParseOptionalLogLevel(values, "logging.console_level", LogLevel::kInfo);
    logging.file_level = ParseOptionalLogLevel(values, "logging.file_level", LogLevel::kDebug);
    logging.flush_level = ParseOptionalLogLevel(values, "logging.flush_level", LogLevel::kError);

    logging.async_queue_size = ParseOptionalUnsigned(values, "logging.async_queue_size", 4096U, 64U, 1048576U);
    logging.async_worker_count = ParseOptionalUnsigned(values, "logging.async_worker_count", 1U, 1U, 8U);
    if (Optional(values, "logging.overflow_policy", "overrun_oldest") != "overrun_oldest") {
        throw ConfigError("logging.overflow_policy must be overrun_oldest");
    }

    logging.file_enabled = ParseOptionalBool(values, "logging.file_enabled", true);
    logging.file_path = ResolvePath(base_directory, Optional(values, "logging.file_path", "logs/runtime.log"));
    logging.file_max_size = ParseOptionalUnsigned(values, "logging.file_max_size", 5242880U, 1024U, 1073741824U);
    logging.file_count = ParseOptionalUnsigned(values, "logging.file_count", 3U, 1U, 1000U);
    if (logging.file_enabled && logging.file_path.empty()) {
        throw ConfigError("logging.file_path must not be empty when file logging is enabled");
    }
}

} // namespace

AppConfig AppConfig::Load(const std::string& path)
{
    if (path.empty()) {
        throw ConfigError("configuration path is empty");
    }

    const IniMap values = ParseIni(path);
    ValidateKnownKeys(values);
    const std::string base_directory = DirectoryName(path);

    AppConfig config;
    config.source_path = path;
    config.can_interface = Required(values, "can.interface");
    ValidateCanInterface(config.can_interface);
    config.can_bitrate = ParseUnsigned(values, "can.bitrate", 10000U, 1000000U);

    config.master_node_id = static_cast<std::uint8_t>(ParseUnsigned(values, "master.node_id", 1U, 127U));
    config.master_dcf_path = ResolvePath(base_directory, Required(values, "master.dcf_path"));
    ValidateRegularFile("master.dcf_path", config.master_dcf_path);

    config.slave_node_id = static_cast<std::uint8_t>(ParseUnsigned(values, "slave.node_id", 1U, 127U));
    if (config.slave_node_id == config.master_node_id) {
        throw ConfigError("master.node_id and slave.node_id must differ");
    }
    config.slave_eds_path = ResolvePath(base_directory, Optional(values, "slave.eds_path", ""));
    if (!config.slave_eds_path.empty()) {
        ValidateRegularFile("slave.eds_path", config.slave_eds_path);
    }

    config.boot_timeout_ms = ParseUnsigned(values, "timeouts.boot_ms", 1U, 600000U);
    config.sdo_timeout_ms = ParseUnsigned(values, "timeouts.sdo_ms", 1U, 600000U);
    config.nmt_state_timeout_ms = ParseUnsigned(values, "timeouts.nmt_state_ms", 1U, 600000U);
    config.pdo_timeout_ms = ParseUnsigned(values, "timeouts.pdo_ms", 1U, 600000U);
    config.lss_timeout_ms = ParseUnsigned(values, "timeouts.lss_ms", 1U, 600000U);

    config.history_path = ResolvePath(base_directory, Optional(values, "console.history_path", "history.txt"));
    config.report_directory = ResolvePath(base_directory, Optional(values, "console.report_directory", "reports"));
    LoadLoggingConfig(values, base_directory, config.logging);
    return config;
}

std::string AppConfig::ToString() const
{
    std::ostringstream stream;
    stream << "config=" << source_path << '\n'
           << "can.interface=" << can_interface << '\n'
           << "can.bitrate=" << can_bitrate << '\n'
           << "master.node_id=" << static_cast<unsigned int>(master_node_id) << '\n'
           << "master.dcf_path=" << master_dcf_path << '\n'
           << "slave.node_id=" << static_cast<unsigned int>(slave_node_id) << '\n'
           << "slave.eds_path=" << slave_eds_path << '\n'
           << "timeouts.boot_ms=" << boot_timeout_ms << '\n'
           << "timeouts.sdo_ms=" << sdo_timeout_ms << '\n'
           << "timeouts.nmt_state_ms=" << nmt_state_timeout_ms << '\n'
           << "timeouts.pdo_ms=" << pdo_timeout_ms << '\n'
           << "timeouts.lss_ms=" << lss_timeout_ms << '\n'
           << "console.history_path=" << history_path << '\n'
           << "console.report_directory=" << report_directory << '\n'
           << "logging.application_level=" << canopen_test::ToString(logging.application_level) << '\n'
           << "logging.configuration_level=" << canopen_test::ToString(logging.configuration_level) << '\n'
           << "logging.runtime_level=" << canopen_test::ToString(logging.runtime_level) << '\n'
           << "logging.socketcan_level=" << canopen_test::ToString(logging.socketcan_level) << '\n'
           << "logging.canopen_level=" << canopen_test::ToString(logging.canopen_level) << '\n'
           << "logging.signal_level=" << canopen_test::ToString(logging.signal_level) << '\n'
           << "logging.lely_level=" << canopen_test::ToString(logging.lely_level) << '\n'
           << "logging.console_level=" << canopen_test::ToString(logging.console_level) << '\n'
           << "logging.file_level=" << canopen_test::ToString(logging.file_level) << '\n'
           << "logging.flush_level=" << canopen_test::ToString(logging.flush_level) << '\n'
           << "logging.async_queue_size=" << logging.async_queue_size << '\n'
           << "logging.async_worker_count=" << logging.async_worker_count << '\n'
           << "logging.overflow_policy=overrun_oldest\n"
           << "logging.file_enabled=" << (logging.file_enabled ? "true" : "false") << '\n'
           << "logging.file_path=" << logging.file_path << '\n'
           << "logging.file_max_size=" << logging.file_max_size << '\n'
           << "logging.file_count=" << logging.file_count;
    return stream.str();
}

} // namespace canopen_test
