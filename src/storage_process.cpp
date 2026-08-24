/**
 * @file
 * @brief J07/B01 CANopenNode EEPROM Storage validation implementation.
 */

#include "storage_process.h"

#include "canopen_config.h"
#include "canopen_nmt.h"
#include "canopen_sdo.h"
#include "nmt_heartbeat.h"

#include <lely/coapp/master.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

/** Standard CANopen Store Parameters object. */
constexpr std::uint16_t kStoreIndex = 0x1010U;
/** Standard CANopen Restore Default Parameters object. */
constexpr std::uint16_t kRestoreIndex = 0x1011U;
/** Persistent Producer Heartbeat probe object. */
constexpr std::uint16_t kProbeIndex = 0x1017U;
/** Local master Consumer Heartbeat Time object. */
constexpr std::uint16_t kHeartbeatConsumerIndex = 0x1016U;
/** The generated master DCF stores node 1 supervision in Consumer entry 1. */
constexpr std::uint8_t kHeartbeatConsumerSubindex = 0x01U;
/** Test-only target Storage diagnostic object. */
constexpr std::uint16_t kDiagnosticIndex = 0x2305U;
/** PERSIST_COMM Storage sub-index used by 0x1010/0x1011. */
constexpr std::uint8_t kCommStorageSubindex = 0x02U;
/** CANopenNode "save" command value in little-endian numeric form. */
constexpr std::uint32_t kSaveMagic = 0x65766173UL;
/** CANopenNode "load" command value in little-endian numeric form. */
constexpr std::uint32_t kRestoreMagic = 0x64616F6CUL;
/** AT24C128 capacity required by the J07 target profile. */
constexpr std::uint32_t kExpectedEepromSize = 16384U;
/** AT24C128 page size required by the J07 target profile. */
constexpr std::uint16_t kExpectedPageSize = 64U;
/** AddrInput 1 selects the supplied AT24CXX adapter's 7-bit address 0x51. */
constexpr std::uint8_t kExpectedAddrInput = 1U;
/** Generated demo OD default for 0x1017 before persistent Storage loading. */
constexpr std::uint16_t kFactoryProbe = 0U;
/** Corruption bit used by CANopenNode for Storage sub-index 2. */
constexpr std::uint32_t kCommCorruptionBit = 1UL << kCommStorageSubindex;
/** Maximum wait for one target diagnostic command. */
constexpr auto kDiagnosticTimeout = std::chrono::milliseconds(10000);
/** Delay between diagnostic completion polls. */
constexpr auto kDiagnosticPoll = std::chrono::milliseconds(20);

/** OD 0x2305 sub-index contract shared with the target diagnostic. */
enum class DiagnosticSubindex : std::uint8_t {
    REQUEST_SEQ = 0x01U, /**< Host commit sequence written after request fields are prepared. */
    COMMAND = 0x02U, /**< Storage diagnostic command selector. */
    ENTRY_SUB_INDEX = 0x03U, /**< Selected 0x1010/0x1011 Storage sub-index; B01 requires 2. */
    RAW_OFFSET = 0x04U, /**< Byte offset relative to the complete diagnostic raw region. */
    RAW_SIZE = 0x05U, /**< Number of raw bytes transferred by one diagnostic command. */
    RAW_VALUE = 0x06U, /**< Little-endian raw read/write payload, up to four bytes. */
    ACTIVE_SEQ = 0x07U, /**< Most recently accepted request sequence. */
    COMPLETE_SEQ = 0x08U, /**< Most recently completed request sequence. */
    RESULT = 0x09U, /**< Normalized terminal result for COMPLETE_SEQ. */
    STARTUP_STATE = 0x0AU, /**< Normalized result of the latest Storage initialization. */
    STARTUP_RESULT = 0x0BU, /**< Raw CO_ReturnError_t from the latest Storage initialization. */
    STARTUP_ERROR = 0x0CU, /**< Corruption bitmask or backend detail from Storage initialization. */
    STORAGE_OFFSET = 0x0DU, /**< Board-configured EEPROM Storage base offset. */
    EEPROM_SIZE = 0x0EU, /**< EEPROM capacity reported by the target adapter. */
    PAGE_SIZE = 0x0FU, /**< EEPROM page size reported by the target adapter. */
    ADDR_INPUT = 0x10U, /**< AT24CXX AddrInput used by the target adapter. */
    SIGNATURE_ADDRESS = 0x11U, /**< EEPROM address of the selected Storage signature. */
    DATA_ADDRESS = 0x12U, /**< EEPROM address of the selected Storage payload. */
    DATA_LENGTH = 0x13U, /**< Selected Storage payload length in bytes. */
    RAW_START = 0x14U, /**< First EEPROM byte included in the complete raw baseline. */
    RAW_LENGTH = 0x15U, /**< Number of EEPROM bytes included in the complete raw baseline. */
    SIGNATURE_VALUE = 0x16U, /**< Current selected-entry CO_storageEeprom signature. */
    BACKUP_VALID = 0x17U, /**< Non-zero when the target-RAM raw backup is valid. */
    BACKUP_CRC = 0x18U, /**< CRC16-CCITT over the target-RAM raw backup. */
    STARTUP_SEQ = 0x19U, /**< Storage initialization capture sequence within the current MCU boot. */
    STARTUP_PROBE = 0x1AU, /**< 0x1017 captured after Storage load and before master Boot writes. */
};

/** Target diagnostic command values. */
enum class DiagnosticCommand : std::uint8_t {
    REFRESH = 1U, /**< Refresh layout and signature metadata without changing EEPROM. */
    BACKUP = 2U, /**< Copy the complete COMM raw region into target RAM. */
    RESTORE = 3U, /**< Restore and verify the target-RAM raw backup. */
    CORRUPT_SIGNATURE = 4U, /**< Flip one CRC bit in the selected Storage signature. */
    CORRUPT_DATA = 5U, /**< Flip one byte in the selected Storage payload. */
    RAW_READ = 6U, /**< Read one to four raw EEPROM bytes. */
    RAW_WRITE = 7U, /**< Write and verify one to four raw EEPROM bytes. */
};

/** Target diagnostic terminal result values. */
enum class DiagnosticResult : std::int32_t {
    SUCCESS = 1, /**< The target diagnostic command completed successfully. */
};

/** Normalized target startup state. */
enum class StartupState : std::uint8_t {
    UNKNOWN = 0U, /**< No Storage initialization result has been captured. */
    OK = 1U, /**< Storage initialization completed without corruption. */
    DATA_CORRUPT = 2U, /**< Storage initialization detected invalid persisted data. */
    ERROR = 3U, /**< Storage initialization failed for a non-corruption reason. */
};

/** Storage layout snapshot read from OD 0x2305. */
struct StorageLayout {
    std::uint32_t storage_offset = 0U; /**< Board-configured Storage base offset. */
    std::uint32_t eeprom_size = 0U; /**< EEPROM capacity in bytes. */
    std::uint16_t page_size = 0U; /**< EEPROM page size in bytes. */
    std::uint8_t addr_input = 0U; /**< AT24CXX AddrInput passed by the target. */
    std::uint32_t signature_address = 0U; /**< Selected entry signature address. */
    std::uint32_t data_address = 0U; /**< Selected entry payload address. */
    std::uint32_t data_length = 0U; /**< Selected entry payload length. */
    std::uint32_t raw_start = 0U; /**< Complete baseline raw-region start. */
    std::uint32_t raw_length = 0U; /**< Complete baseline raw-region length. */
    std::uint32_t signature_value = 0U; /**< Current raw 32-bit signature. */
};

/** Storage initialization snapshot captured before master Boot reconfiguration. */
struct StartupSnapshot {
    StartupState state = StartupState::UNKNOWN; /**< Normalized initialization state. */
    std::int32_t result = 0; /**< Raw CO_ReturnError_t from target Storage init. */
    std::uint32_t error = 0U; /**< Corruption bitmask or backend detail. */
    std::uint32_t sequence = 0U; /**< Target startup-capture sequence. */
    std::uint16_t probe = 0U; /**< Pre-master 0x1017 value loaded from Storage. */
};

/** Complete host-side recovery baseline required before destructive B01 cases. */
struct StorageBaseline {
    StorageLayout layout; /**< Layout associated with raw bytes. */
    StartupSnapshot startup; /**< Initial persisted startup evidence. */
    std::vector<std::uint8_t> raw; /**< Complete signature-table plus COMM payload region. */
    std::uint16_t runtime_probe = 0U; /**< 0x1017 value visible after master Boot configuration. */
    std::uint16_t raw_crc = 0U; /**< Independent CRC16-CCITT over raw baseline bytes. */
    bool captured = false; /**< True only after complete raw capture and CRC cross-check. */
};

/** Convert one diagnostic sub-index enum to its numeric SDO value. */
constexpr std::uint8_t sub(DiagnosticSubindex value) noexcept
{
    return static_cast<std::uint8_t>(value);
}

/** Compute the CANopenNode CRC16-CCITT polynomial with initial CRC supplied by caller. */
std::uint16_t crc16Ccitt(const std::vector<std::uint8_t>& data) noexcept
{
    std::uint16_t crc = 0U;
    for (const std::uint8_t byte : data) {
        crc ^= static_cast<std::uint16_t>(byte) << 8U;
        for (unsigned int bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 0x8000U) != 0U
                ? static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U)
                : static_cast<std::uint16_t>(crc << 1U);
        }
    }
    return crc;
}

/** Read one typed target diagnostic field. */
template <class T>
bool readDiagnostic(lely::canopen::AsyncMaster& master, DiagnosticSubindex field, T& value)
{
    return readRemoteSdo(master, CANOPEN_SLAVE_NODE_ID, kDiagnosticIndex, sub(field), value)
        == SdoOperationResult::SUCCESS;
}

/** Write one typed target diagnostic field. */
template <class T>
bool writeDiagnostic(lely::canopen::AsyncMaster& master, DiagnosticSubindex field, T value)
{
    return writeRemoteSdo(master, CANOPEN_SLAVE_NODE_ID, kDiagnosticIndex, sub(field), value)
        == SdoOperationResult::SUCCESS;
}

/** Commit one target diagnostic command and wait for the matching sequence result. */
bool runDiagnosticCommand(lely::canopen::AsyncMaster& master, DiagnosticCommand command,
                          std::uint32_t raw_offset = 0U, std::uint8_t raw_size = 4U,
                          std::uint32_t raw_value = 0U, std::uint32_t* returned_raw_value = nullptr)
{
    std::uint32_t request_seq = 0U;
    if (!readDiagnostic(master, DiagnosticSubindex::REQUEST_SEQ, request_seq)) {
        return false;
    }
    ++request_seq;
    if (request_seq == 0U) {
        ++request_seq;
    }

    if (!writeDiagnostic(master, DiagnosticSubindex::ENTRY_SUB_INDEX, kCommStorageSubindex)
        || !writeDiagnostic(master, DiagnosticSubindex::RAW_OFFSET, raw_offset)
        || !writeDiagnostic(master, DiagnosticSubindex::RAW_SIZE, raw_size)
        || !writeDiagnostic(master, DiagnosticSubindex::RAW_VALUE, raw_value)
        || !writeDiagnostic(master, DiagnosticSubindex::COMMAND, static_cast<std::uint8_t>(command))
        || !writeDiagnostic(master, DiagnosticSubindex::REQUEST_SEQ, request_seq)) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + kDiagnosticTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::uint32_t complete_seq = 0U;
        if (!readDiagnostic(master, DiagnosticSubindex::COMPLETE_SEQ, complete_seq)) {
            return false;
        }
        if (complete_seq == request_seq) {
            std::int32_t result = 0;
            if (!readDiagnostic(master, DiagnosticSubindex::RESULT, result)) {
                return false;
            }
            if (result != static_cast<std::int32_t>(DiagnosticResult::SUCCESS)) {
                spdlog::error("Storage diagnostic command {} failed: result={}",
                              static_cast<unsigned int>(command), result);
                return false;
            }
            if (returned_raw_value != nullptr
                && !readDiagnostic(master, DiagnosticSubindex::RAW_VALUE, *returned_raw_value)) {
                return false;
            }
            return true;
        }
        std::this_thread::sleep_for(kDiagnosticPoll);
    }

    spdlog::error("Storage diagnostic command {} completion timed out",
                  static_cast<unsigned int>(command));
    return false;
}

/** Refresh and read all layout fields required by host-side backup and assertions. */
bool readLayout(lely::canopen::AsyncMaster& master, StorageLayout& layout)
{
    return runDiagnosticCommand(master, DiagnosticCommand::REFRESH)
        && readDiagnostic(master, DiagnosticSubindex::STORAGE_OFFSET, layout.storage_offset)
        && readDiagnostic(master, DiagnosticSubindex::EEPROM_SIZE, layout.eeprom_size)
        && readDiagnostic(master, DiagnosticSubindex::PAGE_SIZE, layout.page_size)
        && readDiagnostic(master, DiagnosticSubindex::ADDR_INPUT, layout.addr_input)
        && readDiagnostic(master, DiagnosticSubindex::SIGNATURE_ADDRESS, layout.signature_address)
        && readDiagnostic(master, DiagnosticSubindex::DATA_ADDRESS, layout.data_address)
        && readDiagnostic(master, DiagnosticSubindex::DATA_LENGTH, layout.data_length)
        && readDiagnostic(master, DiagnosticSubindex::RAW_START, layout.raw_start)
        && readDiagnostic(master, DiagnosticSubindex::RAW_LENGTH, layout.raw_length)
        && readDiagnostic(master, DiagnosticSubindex::SIGNATURE_VALUE, layout.signature_value);
}

/** Read the target's pre-master Storage initialization snapshot. */
bool readStartup(lely::canopen::AsyncMaster& master, StartupSnapshot& startup)
{
    std::uint8_t state = 0U;
    if (!readDiagnostic(master, DiagnosticSubindex::STARTUP_STATE, state)
        || !readDiagnostic(master, DiagnosticSubindex::STARTUP_RESULT, startup.result)
        || !readDiagnostic(master, DiagnosticSubindex::STARTUP_ERROR, startup.error)
        || !readDiagnostic(master, DiagnosticSubindex::STARTUP_SEQ, startup.sequence)
        || !readDiagnostic(master, DiagnosticSubindex::STARTUP_PROBE, startup.probe)) {
        return false;
    }
    startup.state = static_cast<StartupState>(state);
    return true;
}

/** Validate the AT24C128 geometry and contiguous CO_storageEeprom entry layout. */
bool validateLayout(const StorageLayout& layout)
{
    const std::uint64_t raw_end = static_cast<std::uint64_t>(layout.raw_start) + layout.raw_length;
    const std::uint64_t data_end = static_cast<std::uint64_t>(layout.data_address) + layout.data_length;
    if (layout.eeprom_size != kExpectedEepromSize || layout.page_size != kExpectedPageSize
        || layout.addr_input != kExpectedAddrInput || layout.raw_length == 0U
        || layout.storage_offset != layout.signature_address || layout.raw_start != layout.storage_offset
        || layout.data_address < layout.signature_address || data_end != raw_end
        || raw_end > layout.eeprom_size || layout.data_length > 0xFFFFU) {
        spdlog::error(
            "B01-01 Storage layout mismatch: offset=0x{:x} size={} page={} addrInput={} "
            "sig=0x{:x} data=0x{:x}/{} raw=0x{:x}/{}",
            layout.storage_offset, layout.eeprom_size, layout.page_size,
            static_cast<unsigned int>(layout.addr_input), layout.signature_address,
            layout.data_address, layout.data_length, layout.raw_start, layout.raw_length);
        return false;
    }
    spdlog::info(
        "B01-01 passed: AT24C128 size={} page={} AddrInput={} (7-bit address 0x51), "
        "Storage raw=[0x{:x},0x{:x})",
        layout.eeprom_size, layout.page_size, static_cast<unsigned int>(layout.addr_input),
        layout.raw_start, static_cast<std::uint32_t>(raw_end));
    return true;
}

/** Read the complete diagnostic raw region in 1..4-byte SDO transactions. */
bool readRawRegion(lely::canopen::AsyncMaster& master, const StorageLayout& layout,
                   std::vector<std::uint8_t>& raw)
{
    raw.assign(layout.raw_length, 0U);
    for (std::uint32_t offset = 0U; offset < layout.raw_length;) {
        const std::uint8_t chunk = static_cast<std::uint8_t>(
            std::min<std::uint32_t>(4U, layout.raw_length - offset));
        std::uint32_t word = 0U;
        if (!runDiagnosticCommand(master, DiagnosticCommand::RAW_READ, offset, chunk, 0U, &word)) {
            return false;
        }
        for (std::uint8_t i = 0U; i < chunk; ++i) {
            raw[offset + i] = static_cast<std::uint8_t>(word >> (8U * i));
        }
        offset += chunk;
    }
    return true;
}

/** Write the complete raw baseline and verify it byte-for-byte. */
bool restoreRawRegion(lely::canopen::AsyncMaster& master, const StorageBaseline& baseline)
{
    for (std::uint32_t offset = 0U; offset < baseline.layout.raw_length;) {
        const std::uint8_t chunk = static_cast<std::uint8_t>(
            std::min<std::uint32_t>(4U, baseline.layout.raw_length - offset));
        std::uint32_t word = 0U;
        for (std::uint8_t i = 0U; i < chunk; ++i) {
            word |= static_cast<std::uint32_t>(baseline.raw[offset + i]) << (8U * i);
        }
        if (!runDiagnosticCommand(master, DiagnosticCommand::RAW_WRITE, offset, chunk, word)) {
            return false;
        }
        offset += chunk;
    }

    std::vector<std::uint8_t> verify;
    return readRawRegion(master, baseline.layout, verify) && verify == baseline.raw;
}

/** Capture a full host baseline only after target-side backup has succeeded. */
bool captureBaseline(lely::canopen::AsyncMaster& master, StorageBaseline& baseline)
{
    if (!readLayout(master, baseline.layout) || !validateLayout(baseline.layout)
        || !readStartup(master, baseline.startup)
        || readRemoteSdo(master, CANOPEN_SLAVE_NODE_ID, kProbeIndex, 0x00U, baseline.runtime_probe)
            != SdoOperationResult::SUCCESS
        || !runDiagnosticCommand(master, DiagnosticCommand::BACKUP)) {
        return false;
    }

    std::uint8_t target_backup_valid = 0U;
    std::uint16_t target_backup_crc = 0U;
    if (!readDiagnostic(master, DiagnosticSubindex::BACKUP_VALID, target_backup_valid)
        || !readDiagnostic(master, DiagnosticSubindex::BACKUP_CRC, target_backup_crc)
        || target_backup_valid == 0U || !readRawRegion(master, baseline.layout, baseline.raw)) {
        return false;
    }

    baseline.raw_crc = crc16Ccitt(baseline.raw);
    if (baseline.raw_crc != target_backup_crc) {
        spdlog::error("B01-03 baseline CRC mismatch: host=0x{:04x} target=0x{:04x}",
                      baseline.raw_crc, target_backup_crc);
        return false;
    }
    baseline.captured = true;
    spdlog::info("B01-03 passed: complete raw baseline captured, len={} crc=0x{:04x}",
                 baseline.raw.size(), baseline.raw_crc);
    return true;
}

/** Temporarily disable local Heartbeat supervision during one expected slave outage. */
bool suspendHeartbeatConsumer(lely::canopen::AsyncMaster& master,
                              std::uint32_t& saved_consumer)
{
    std::error_code error;
    saved_consumer = master.Read<std::uint32_t>(
        kHeartbeatConsumerIndex, kHeartbeatConsumerSubindex, error);
    if (error) {
        spdlog::error(
            "B01 unable to read local 0x1016:{:02x} before expected outage: {}",
            static_cast<unsigned int>(kHeartbeatConsumerSubindex),
            error.message());
        return false;
    }

    master.Write<std::uint32_t>(kHeartbeatConsumerIndex,
                                kHeartbeatConsumerSubindex, 0U, error);
    if (error) {
        spdlog::error(
            "B01 unable to suspend local 0x1016:{:02x} before expected outage: {}",
            static_cast<unsigned int>(kHeartbeatConsumerSubindex),
            error.message());
        return false;
    }

    spdlog::info(
        "B01 expected outage window started: local 0x1016:{:02x} "
        "0x{:08x} -> 0",
        static_cast<unsigned int>(kHeartbeatConsumerSubindex),
        saved_consumer);
    return true;
}

/** Restore local Heartbeat supervision after one expected slave outage. */
bool restoreHeartbeatConsumer(lely::canopen::AsyncMaster& master,
                              std::uint32_t saved_consumer)
{
    std::error_code error;
    master.Write<std::uint32_t>(kHeartbeatConsumerIndex,
                                kHeartbeatConsumerSubindex, saved_consumer,
                                error);
    if (error) {
        spdlog::error(
            "B01 unable to restore local 0x1016:{:02x}=0x{:08x}: {}",
            static_cast<unsigned int>(kHeartbeatConsumerSubindex),
            saved_consumer, error.message());
        return false;
    }

    spdlog::info(
        "B01 expected outage window ended: local 0x1016:{:02x} restored "
        "to 0x{:08x}",
        static_cast<unsigned int>(kHeartbeatConsumerSubindex),
        saved_consumer);
    return true;
}

/** Reset the MCU node once and require a fresh Lely Boot callback. */
bool resetNodeAndWait(lely::canopen::AsyncMaster& master, const char* description,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS))
{
    std::uint32_t saved_consumer = 0U;
    if (!suspendHeartbeatConsumer(master, saved_consumer)) {
        return false;
    }

    prepareBootWait();
    const bool reset_completed =
        issueNmtCommand(master, lely::canopen::NmtCommand::RESET_NODE,
                        CANOPEN_SLAVE_NODE_ID, description)
        && waitForBootCompletion(timeout);
    const bool supervision_restored =
        restoreHeartbeatConsumer(master, saved_consumer);

    if (!reset_completed) {
        spdlog::error("{} did not complete a fresh Boot", description);
    }
    return reset_completed && supervision_restored;
}

/** Read and validate standard 0x1010:02/0x1011:02 capability values. */
bool validateStorageObjects(lely::canopen::AsyncMaster& master)
{
    std::uint32_t save_capability = 0U;
    std::uint32_t restore_capability = 0U;
    if (readRemoteSdo(master, CANOPEN_SLAVE_NODE_ID, kStoreIndex, kCommStorageSubindex, save_capability)
            != SdoOperationResult::SUCCESS
        || readRemoteSdo(master, CANOPEN_SLAVE_NODE_ID, kRestoreIndex, kCommStorageSubindex,
                         restore_capability) != SdoOperationResult::SUCCESS
        || (save_capability & 1U) == 0U || (restore_capability & 1U) == 0U) {
        spdlog::error("B01-02 0x1010:02/0x1011:02 capability check failed: save=0x{:08x} restore=0x{:08x}",
                      save_capability, restore_capability);
        return false;
    }
    spdlog::info("B01-02 passed: save=0x{:08x} restore=0x{:08x}", save_capability, restore_capability);
    return true;
}

/** Select a nonzero probe that differs from one baseline value. */
std::uint16_t chooseProbe(std::uint16_t baseline, std::uint16_t preferred) noexcept
{
    return baseline == preferred ? static_cast<std::uint16_t>(preferred ^ 0x0111U) : preferred;
}

/** Write 0x1017 and invoke 0x1010:02 save. */
bool saveProbe(lely::canopen::AsyncMaster& master, std::uint16_t probe)
{
    return writeRemoteSdo(master, CANOPEN_SLAVE_NODE_ID, kProbeIndex, 0x00U, probe)
            == SdoOperationResult::SUCCESS
        && writeRemoteSdo(master, CANOPEN_SLAVE_NODE_ID, kStoreIndex, kCommStorageSubindex, kSaveMagic)
            == SdoOperationResult::SUCCESS;
}

/** Require one startup snapshot state/probe after a reset or power cycle. */
bool expectStartup(lely::canopen::AsyncMaster& master, StartupState state,
                   std::uint16_t probe, bool require_comm_corruption)
{
    StartupSnapshot startup;
    if (!readStartup(master, startup) || startup.state != state || startup.probe != probe
        || (require_comm_corruption && (startup.error & kCommCorruptionBit) == 0U)) {
        spdlog::error("Storage startup mismatch: state={} result={} error=0x{:08x} probe={} expectedState={} expectedProbe={}",
                      static_cast<unsigned int>(startup.state), startup.result, startup.error, startup.probe,
                      static_cast<unsigned int>(state), probe);
        return false;
    }
    return true;
}

/** Require the restored raw image to reproduce the original startup snapshot. */
bool expectBaselineStartup(lely::canopen::AsyncMaster& master, const StartupSnapshot& baseline)
{
    StartupSnapshot current;
    if (!readStartup(master, current) || current.state != baseline.state
        || current.result != baseline.result || current.error != baseline.error
        || current.probe != baseline.probe) {
        spdlog::error(
            "Baseline startup mismatch: state={} result={} error=0x{:08x} probe={} "
            "expectedState={} expectedResult={} expectedError=0x{:08x} expectedProbe={}",
            static_cast<unsigned int>(current.state), current.result, current.error, current.probe,
            static_cast<unsigned int>(baseline.state), baseline.result, baseline.error, baseline.probe);
        return false;
    }
    return true;
}

/** Restore raw persistence and reload the complete runtime baseline; DIRTY is terminal. */
bool cleanupBaseline(lely::canopen::AsyncMaster& master, const StorageBaseline& baseline)
{
    if (!baseline.captured || !restoreRawRegion(master, baseline)
        || !resetNodeAndWait(master, "B01 cleanup Reset Node")
        || !expectBaselineStartup(master, baseline.startup)
        || writeRemoteSdo(master, CANOPEN_SLAVE_NODE_ID, kProbeIndex, 0x00U, baseline.runtime_probe)
            != SdoOperationResult::SUCCESS) {
        spdlog::critical("B01 cleanup failed; environment=DIRTY. Stop all later destructive Storage stages.");
        return false;
    }
    spdlog::info("B01 cleanup verified after reload; environment=CLEAN");
    return true;
}

/** B01-01..B01-07: core layout, backup/restore, save and reset persistence. */
int runCoreMode(lely::canopen::AsyncMaster& master)
{
    StorageLayout layout;
    if (!readLayout(master, layout) || !validateLayout(layout) || !validateStorageObjects(master)) {
        return 1;
    }

    StorageBaseline baseline;
    if (!captureBaseline(master, baseline)) {
        return 1;
    }
    bool passed = true;

    /* B01-04 proves target-RAM BACKUP/RESTORE covers every raw byte, not only OD data. */
    const std::uint32_t data_offset = baseline.layout.data_address - baseline.layout.raw_start;
    const std::uint8_t mutated = static_cast<std::uint8_t>(baseline.raw[data_offset] ^ 0x01U);
    if (!runDiagnosticCommand(master, DiagnosticCommand::RAW_WRITE, data_offset, 1U, mutated)
        || !runDiagnosticCommand(master, DiagnosticCommand::RESTORE)) {
        passed = false;
    } else {
        std::vector<std::uint8_t> restored;
        passed = readRawRegion(master, baseline.layout, restored) && restored == baseline.raw;
    }
    spdlog::info("B01-04 {}: target raw backup/restore", passed ? "passed" : "failed");

    const std::uint16_t probe1 = chooseProbe(baseline.runtime_probe, 0x0555U);
    const std::uint16_t probe2 = chooseProbe(probe1, 0x0666U);
    if (passed && saveProbe(master, probe1)) {
        StorageLayout saved_layout;
        passed = readLayout(master, saved_layout)
            && (saved_layout.signature_value & 0xFFFFU) == saved_layout.data_length;
    } else {
        passed = false;
    }
    spdlog::info("B01-05 {}: 0x1017 + 0x1010:02 save", passed ? "passed" : "failed");

    if (passed) {
        passed = resetNodeAndWait(master, "B01-06 Reset Node")
            && expectStartup(master, StartupState::OK, probe1, false);
    }
    spdlog::info("B01-06 {}: reset persistence via pre-master startup probe", passed ? "passed" : "failed");

    if (passed) {
        passed = saveProbe(master, probe2)
            && resetNodeAndWait(master, "B01-07 Reset Node")
            && expectStartup(master, StartupState::OK, probe2, false);
    }
    spdlog::info("B01-07 {}: repeated save replaces persistent value", passed ? "passed" : "failed");

    const bool cleaned = cleanupBaseline(master, baseline);
    return passed && cleaned ? 0 : 1;
}

/** B01-08..B01-10: restore-default signature invalidation and baseline recovery. */
int runRestoreMode(lely::canopen::AsyncMaster& master)
{
    StorageBaseline baseline;
    if (!captureBaseline(master, baseline)) {
        return 1;
    }
    bool passed = true;
    const std::uint16_t probe = chooseProbe(baseline.runtime_probe, 0x0777U);

    if (!saveProbe(master, probe)
        || writeRemoteSdo(master, CANOPEN_SLAVE_NODE_ID, kRestoreIndex, kCommStorageSubindex, kRestoreMagic)
            != SdoOperationResult::SUCCESS) {
        passed = false;
    } else {
        StorageLayout layout;
        passed = readLayout(master, layout) && layout.signature_value == 0xFFFFFFFFUL;
    }
    spdlog::info("B01-08 {}: 0x1011:02 invalidates the COMM signature", passed ? "passed" : "failed");

    if (passed) {
        passed = resetNodeAndWait(master, "B01-09 Reset Node after restore defaults")
            && expectStartup(master, StartupState::DATA_CORRUPT, kFactoryProbe, true);
    }
    spdlog::info("B01-09 {}: erased signature is detected at startup", passed ? "passed" : "failed");

    const bool cleaned = cleanupBaseline(master, baseline);
    spdlog::info("B01-10 {}: full original raw baseline restored", cleaned ? "passed" : "failed");
    return passed && cleaned ? 0 : 1;
}

/** Re-establish a known-valid temporary Storage image before one corruption injection. */
bool stageValidImage(lely::canopen::AsyncMaster& master, std::uint16_t probe)
{
    return saveProbe(master, probe);
}

/** B01-11..B01-13: signature and payload corruption detection plus recovery. */
int runCorruptionMode(lely::canopen::AsyncMaster& master)
{
    StorageBaseline baseline;
    if (!captureBaseline(master, baseline)) {
        return 1;
    }
    bool passed = true;
    const std::uint16_t signature_probe = chooseProbe(baseline.runtime_probe, 0x0888U);
    const std::uint16_t data_probe = chooseProbe(signature_probe, 0x0999U);

    if (!stageValidImage(master, signature_probe)
        || !runDiagnosticCommand(master, DiagnosticCommand::CORRUPT_SIGNATURE)
        || !resetNodeAndWait(master, "B01-11 Reset Node after signature corruption")
        || !expectStartup(master, StartupState::DATA_CORRUPT, kFactoryProbe, true)) {
        passed = false;
    }
    spdlog::info("B01-11 {}: signature corruption detected", passed ? "passed" : "failed");

    if (passed) {
        /* Restore the exact initial image before staging the independent data-corruption case. */
        passed = restoreRawRegion(master, baseline)
            && stageValidImage(master, data_probe);
    }
    if (passed) {
        StorageLayout layout;
        passed = readLayout(master, layout);
        if (passed) {
            const std::uint32_t data_offset = layout.data_address - layout.raw_start;
            passed = runDiagnosticCommand(master, DiagnosticCommand::CORRUPT_DATA, data_offset, 1U)
                && resetNodeAndWait(master, "B01-12 Reset Node after payload corruption")
                && expectStartup(master, StartupState::DATA_CORRUPT, data_probe, true);
        }
    }
    spdlog::info("B01-12 {}: payload CRC corruption detected", passed ? "passed" : "failed");

    const bool cleaned = cleanupBaseline(master, baseline);
    spdlog::info("B01-13 {}: corruption cleanup restored complete baseline", cleaned ? "passed" : "failed");
    return passed && cleaned ? 0 : 1;
}

/** Wait for one operator-driven power cycle without automatic Heartbeat recovery reset. */
bool waitForOperatorPowerCycle(lely::canopen::AsyncMaster& master,
                               const char* case_name)
{
    std::uint32_t saved_consumer = 0U;
    if (!suspendHeartbeatConsumer(master, saved_consumer)) {
        return false;
    }

    prepareBootWait();
    spdlog::warn("{} ACTION REQUIRED: power-cycle the MCU now", case_name);
    const bool boot_completed = waitForBootCompletion(
        std::chrono::milliseconds(CANOPEN_STORAGE_OPERATOR_TIMEOUT_MS));
    const bool supervision_restored =
        restoreHeartbeatConsumer(master, saved_consumer);
    return boot_completed && supervision_restored;
}

/** B01-14..B01-15: persisted save and baseline recovery across real power cycles. */
int runPowerCycleMode(lely::canopen::AsyncMaster& master)
{
    StorageBaseline baseline;
    if (!captureBaseline(master, baseline)) {
        return 1;
    }
    bool passed = true;
    const std::uint16_t probe = chooseProbe(baseline.runtime_probe, 0x0AAAU);

    if (!saveProbe(master, probe)) {
        passed = false;
    } else {
        passed = waitForOperatorPowerCycle(master, "B01-14")
            && expectStartup(master, StartupState::OK, probe, false);
    }
    spdlog::info("B01-14 {}: save survived a physical power cycle", passed ? "passed" : "failed");

    if (passed) {
        passed = restoreRawRegion(master, baseline);
    }
    if (passed) {
        passed = waitForOperatorPowerCycle(master, "B01-15")
            && expectStartup(master, baseline.startup.state, baseline.startup.probe,
                             baseline.startup.state == StartupState::DATA_CORRUPT);
    }
    spdlog::info("B01-15 {}: baseline survived the recovery power cycle", passed ? "passed" : "failed");

    const bool cleaned = cleanupBaseline(master, baseline);
    return passed && cleaned ? 0 : 1;
}

/** B01-16: operator cuts power during 0x1010 save and result is classified after reboot. */
int runPowerInterruptionMode(lely::canopen::AsyncMaster& master)
{
    StorageBaseline baseline;
    if (!captureBaseline(master, baseline)) {
        return 1;
    }

    const std::uint16_t old_probe = chooseProbe(baseline.runtime_probe, 0x0BBBU);
    const std::uint16_t new_probe = chooseProbe(old_probe, 0x0CCCU);
    bool passed = stageValidImage(master, old_probe);
    if (passed) {
        passed = writeRemoteSdo(master, CANOPEN_SLAVE_NODE_ID, kProbeIndex, 0x00U, new_probe)
            == SdoOperationResult::SUCCESS;
    }

    SdoOperationResult save_result = SdoOperationResult::FAILED;
    if (passed) {
        std::uint32_t saved_consumer = 0U;
        if (!suspendHeartbeatConsumer(master, saved_consumer)) {
            passed = false;
        } else {
            prepareBootWait();
            spdlog::warn(
                "B01-16 ACTION REQUIRED: cut MCU power during the following 0x1010 save, then restore power");
            save_result = writeRemoteSdo(master, CANOPEN_SLAVE_NODE_ID, kStoreIndex,
                                         kCommStorageSubindex, kSaveMagic);
            if (save_result == SdoOperationResult::SUCCESS) {
                spdlog::error("B01-16 save completed before power interruption was observed");
                passed = false;
            } else if (!waitForBootCompletion(
                           std::chrono::milliseconds(CANOPEN_STORAGE_OPERATOR_TIMEOUT_MS))) {
                spdlog::error("B01-16 no fresh Boot observed after the interrupted save");
                passed = false;
            }
            if (!restoreHeartbeatConsumer(master, saved_consumer)) {
                passed = false;
            }
        }
    }

    if (passed) {
        StartupSnapshot startup;
        passed = readStartup(master, startup);
        if (passed && startup.state == StartupState::DATA_CORRUPT) {
            passed = (startup.error & kCommCorruptionBit) != 0U;
        } else if (passed && startup.state == StartupState::OK) {
            passed = startup.probe == old_probe || startup.probe == new_probe;
        } else {
            passed = false;
        }
        if (!passed) {
            spdlog::error("B01-16 post-interruption Storage image was neither detected corrupt nor a valid old/new commit");
        }
    }

    const bool cleaned = cleanupBaseline(master, baseline);
    spdlog::info("B01-16 {}: power-interruption outcome classified and baseline restored",
                 passed && cleaned ? "passed" : "failed");
    return passed && cleaned ? 0 : 1;
}

} // namespace

int storageProcess(lely::canopen::AsyncMaster& master)
{
    const char* mode_env = std::getenv(CANOPEN_STORAGE_MODE_ENV);
    const std::string mode = mode_env == nullptr ? "core" : mode_env;
    spdlog::info("J07/B01 Storage process started: mode={}", mode);

    if (mode == "core") {
        return runCoreMode(master);
    }
    if (mode == "restore") {
        return runRestoreMode(master);
    }
    if (mode == "corruption") {
        return runCorruptionMode(master);
    }
    if (mode == "power-cycle") {
        return runPowerCycleMode(master);
    }
    if (mode == "power-interruption") {
        return runPowerInterruptionMode(master);
    }

    spdlog::error(
        "Unsupported {}='{}'. Expected core, restore, corruption, power-cycle or power-interruption",
        CANOPEN_STORAGE_MODE_ENV, mode);
    return 1;
}
