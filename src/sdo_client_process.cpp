/**
 * @file
 * @brief Implements MCU SDO client validation.
 */

#include "sdo_client_process.h"

#include "canopen_config.h"
#include "canopen_master.h"
#include "canopen_nmt.h"
#include "canopen_sdo.h"
#include "host_sdo_server_fixture.h"
#include "nmt_heartbeat.h"

#include <lely/coapp/node.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

/** MCU test-only SDO Client control/status record. */
constexpr std::uint16_t kControlIndex = 0x2303U;
/** Commit sequence for a new MCU transaction. */
constexpr std::uint8_t kRequestSeqSub = 0x01U;
/** Requested transaction command. */
constexpr std::uint8_t kCommandSub = 0x02U;
/** SDO server node-ID selected by the MCU client. */
constexpr std::uint8_t kRemoteNodeSub = 0x03U;
/** Remote Object Dictionary index. */
constexpr std::uint8_t kIndexSub = 0x04U;
/** Remote Object Dictionary sub-index. */
constexpr std::uint8_t kSubIndexSub = 0x05U;
/** DOWNLOAD payload size; zero for UPLOAD requests. */
constexpr std::uint8_t kPayloadSizeSub = 0x06U;
/** U32 probe value or deterministic segmented payload seed. */
constexpr std::uint8_t kProbeValueSub = 0x07U;
/** MCU SDO client / SDO block-transfer validation request flags. */
constexpr std::uint8_t kFlagsSub = 0x08U;
/** Sequence accepted by the MCU mainline. */
constexpr std::uint8_t kActiveSeqSub = 0x09U;
/** Sequence whose terminal result has been published. */
constexpr std::uint8_t kCompleteSeqSub = 0x0AU;
/** MCU test-wrapper terminal result. */
constexpr std::uint8_t kResultSub = 0x0BU;
/** Native CANopen SDO abort code from the completed transaction. */
constexpr std::uint8_t kAbortCodeSub = 0x0CU;
/** Number of payload bytes transferred. */
constexpr std::uint8_t kTransferredSizeSub = 0x0DU;
/** Uploaded U32 value, or the U32 DOWNLOAD probe on success. */
constexpr std::uint8_t kResultValueSub = 0x0EU;
/** FNV-1a checksum over the transferred payload. */
constexpr std::uint8_t kChecksumSub = 0x0FU;

/** MCU local U32 object used by MCU SDO client validation local-transfer cases. */
constexpr std::uint16_t kLocalControlIndex = 0x2200U;
/** Deliberately nonexistent local object used for abort validation. */
constexpr std::uint16_t kMissingLocalIndex = 0x2FFFU;
/** Mandatory Device Type object used to probe the timeout-test Node-ID. */
constexpr std::uint16_t kDeviceTypeIndex = 0x1000U;
/** Node-ID intentionally left without an SDO server for timeout tests. */
constexpr std::uint8_t kMissingNodeId = 126U;
/** First SDO Client operation command value. */
constexpr std::uint8_t kUploadCommand = 1U;
/** Second SDO Client operation command value. */
constexpr std::uint8_t kDownloadCommand = 2U;
/** MCU SDO client validation segmented/expedited requests keep all optional flags clear. */
constexpr std::uint8_t kNoFlags = 0U;
/** MCU SDO client block-transfer regression requests CANopenNode SDO Client block transfer. */
constexpr std::uint8_t kBlockFlag = 0x01U;
/** U32 payload length. */
constexpr std::uint32_t kU32Size = 4U;
/** Payload larger than both expedited data and the configured 32-byte FIFO. */
constexpr std::uint32_t kSegmentedSize = 48U;
/** Deterministic segmented pattern seed. */
constexpr std::uint32_t kSegmentedSeed = 0xA1B2C3D4U;
/** MCU SDO client block-transfer regression block payload size served by the Host fixture. */
constexpr std::uint32_t kBlockRegressionSize = 2048U;
/** Deterministic MCU SDO client block-transfer regression block payload seed. */
constexpr std::uint32_t kBlockRegressionSeed = 0xB0212C3DU;
/** Primary reversible U32 probe. */
constexpr std::uint32_t kProbeValue = 0x12345678U;
/** Alternate value used when the saved value equals the primary probe. */
constexpr std::uint32_t kAlternateProbeValue = 0x87654321U;
/** Maximum time for a normal MCU transaction to publish completion. */
constexpr std::uint32_t kCompletionTimeoutMs = 2000U;
/** Maximum time for the MCU to accept a request before reset injection. */
constexpr std::uint32_t kActiveTimeoutMs = 500U;
/** Host-side SDO timeout used to prove the timeout-test node is unresponsive. */
constexpr std::uint32_t kMissingNodeProbeTimeoutMs = 500U;
/** Extra callback margin for the Host-side missing-node probe. */
constexpr std::uint32_t kMissingNodeProbeMarginMs = 250U;
/** Poll interval for the 0x2303 status record. */
constexpr std::uint32_t kPollIntervalMs = 20U;
/** FNV-1a 32-bit offset basis shared with the MCU test wrapper. */
constexpr std::uint32_t kFnvOffset = 2166136261UL;
/** FNV-1a 32-bit prime shared with the MCU test wrapper. */
constexpr std::uint32_t kFnvPrime = 16777619UL;
/** CiA 301 SDO protocol timeout abort code. */
constexpr std::uint32_t kAbortTimeout = 0x05040000UL;
/** CiA 301 read-only object abort code. */
constexpr std::uint32_t kAbortReadOnly = 0x06010002UL;
/** CiA 301 object-does-not-exist abort code. */
constexpr std::uint32_t kAbortNotExist = 0x06020000UL;

/** Terminal result values published by the MCU 0x2303 contract. */
enum class McuSdoClientResult : std::int32_t {
    NONE = 0, /**< No completed request is currently represented. */
    SUCCESS = 1, /**< SDO Client transaction completed successfully. */
    ABORT = 2, /**< SDO Client ended with a non-timeout abort. */
    TIMEOUT = 3, /**< SDO Client ended with CO_SDO_AB_TIMEOUT. */
    RESET_CANCELLED = 4, /**< Communication reset cancelled the active request. */
    SETUP_ERROR = 5, /**< Request could not initialize the native client. */
    UNSUPPORTED = 6, /**< Request uses an unavailable or unsupported test option. */
    INTERNAL_ERROR = 7, /**< Wrapper observed an inconsistent transfer result. */
};

/** One immutable request written before request_seq commits it. */
struct ClientRequest {
    std::uint8_t command = 0U; /**< Upload/download command. */
    std::uint8_t remote_node = 0U; /**< Target SDO server node-ID. */
    std::uint16_t index = 0U; /**< Target OD index. */
    std::uint8_t subindex = 0U; /**< Target OD sub-index. */
    std::uint32_t payload_size = 0U; /**< DOWNLOAD byte count. */
    std::uint32_t probe_value = 0U; /**< U32 value or payload seed. */
    std::uint8_t flags = kNoFlags; /**< Optional request-mode flags. */
};

/** Coherent terminal status read from the MCU control record. */
struct ClientStatus {
    std::uint32_t active_seq = 0U; /**< Accepted request sequence. */
    std::uint32_t complete_seq = 0U; /**< Completed request sequence. */
    std::int32_t result = 0; /**< McuSdoClientResult numeric value. */
    std::uint32_t abort_code = 0U; /**< Native SDO abort code. */
    std::uint32_t transferred_size = 0U; /**< Actual payload byte count. */
    std::uint32_t result_value = 0U; /**< U32 result value. */
    std::uint32_t checksum = 0U; /**< Payload FNV-1a checksum. */
};

/** Return one deterministic payload byte for the shared test pattern. */
std::uint8_t patternByte(std::uint32_t seed, std::uint32_t offset) noexcept
{
    const std::uint8_t seed_byte = static_cast<std::uint8_t>(
        seed >> ((offset & 0x03U) * 8U));
    return static_cast<std::uint8_t>(seed_byte
                                     ^ static_cast<std::uint8_t>(offset));
}

/** Build one deterministic fixture payload. */
std::vector<std::uint8_t> makePayload(std::uint32_t size, std::uint32_t seed)
{
    std::vector<std::uint8_t> data(size);
    for (std::uint32_t i = 0U; i < size; ++i) {
        data[i] = patternByte(seed, i);
    }
    return data;
}

/** Build the 48-byte segmented fixture payload. */
std::vector<std::uint8_t> makeSegmentedPayload(std::uint32_t seed)
{
    return makePayload(kSegmentedSize, seed);
}

/** Calculate the checksum reported by the MCU for one payload. */
std::uint32_t checksum(const std::vector<std::uint8_t>& data) noexcept
{
    std::uint32_t value = kFnvOffset;
    for (const std::uint8_t byte : data) {
        value ^= static_cast<std::uint32_t>(byte);
        value *= kFnvPrime;
    }
    return value;
}

/** Read one typed field from the MCU 0x2303 record. */
template <class T>
bool readControl(CanopenTestMaster& master, std::uint8_t subindex, T& value)
{
    return readRemoteSdo<T>(master, CANOPEN_SLAVE_NODE_ID, kControlIndex,
                            subindex, value) == SdoOperationResult::SUCCESS;
}

/** Write one typed request field to the MCU 0x2303 record. */
template <class T>
bool writeControl(CanopenTestMaster& master, std::uint8_t subindex, T value)
{
    return writeRemoteSdo<T>(master, CANOPEN_SLAVE_NODE_ID, kControlIndex,
                             subindex, value) == SdoOperationResult::SUCCESS;
}

/** Allocate a new nonzero request sequence from the current MCU record. */
bool nextRequestSequence(CanopenTestMaster& master, std::uint32_t& sequence)
{
    if (!readControl(master, kRequestSeqSub, sequence)) {
        return false;
    }
    ++sequence;
    if (sequence == 0U) {
        sequence = 1U;
    }
    return true;
}

/** Write immutable request fields, committing request_seq last. */
bool submitRequest(CanopenTestMaster& master, const ClientRequest& request,
                   std::uint32_t sequence)
{
    if (!writeControl(master, kCommandSub, request.command)
        || !writeControl(master, kRemoteNodeSub, request.remote_node)
        || !writeControl(master, kIndexSub, request.index)
        || !writeControl(master, kSubIndexSub, request.subindex)
        || !writeControl(master, kPayloadSizeSub, request.payload_size)
        || !writeControl(master, kProbeValueSub, request.probe_value)
        || !writeControl(master, kFlagsSub, request.flags)) {
        spdlog::error("MCU SDO client validation request {} setup failed before commit", sequence);
        return false;
    }

    if (!writeControl(master, kRequestSeqSub, sequence)) {
        spdlog::error(
            "MCU SDO client validation request {} commit failed; inspect 0x2303 before retrying",
            sequence);
        return false;
    }
    return true;
}

/** Read a result snapshot guarded by complete_seq before and after fields. */
bool readStatus(CanopenTestMaster& master, ClientStatus& status)
{
    for (unsigned int attempt = 0U; attempt < 3U; ++attempt) {
        std::uint32_t complete_before = 0U;
        std::uint32_t complete_after = 0U;
        ClientStatus candidate;

        if (!readControl(master, kCompleteSeqSub, complete_before)
            || !readControl(master, kActiveSeqSub, candidate.active_seq)
            || !readControl(master, kResultSub, candidate.result)
            || !readControl(master, kAbortCodeSub, candidate.abort_code)
            || !readControl(master, kTransferredSizeSub,
                            candidate.transferred_size)
            || !readControl(master, kResultValueSub, candidate.result_value)
            || !readControl(master, kChecksumSub, candidate.checksum)
            || !readControl(master, kCompleteSeqSub, complete_after)) {
            return false;
        }
        if (complete_before == complete_after) {
            candidate.complete_seq = complete_after;
            status = candidate;
            return true;
        }
    }

    spdlog::error("MCU SDO client validation unable to obtain a stable 0x2303 result snapshot");
    return false;
}

/** Poll one U32 control field until it equals the committed sequence. */
bool waitSequence(CanopenTestMaster& master, std::uint8_t subindex,
                  std::uint32_t sequence, std::uint32_t timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms);
    do {
        std::uint32_t observed = 0U;
        if (!readControl(master, subindex, observed)) {
            return false;
        }
        if (observed == sequence) {
            return true;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kPollIntervalMs));
    } while (std::chrono::steady_clock::now() < deadline);

    return false;
}

/** Wait until the request is accepted but has not yet completed. */
bool waitActivePending(CanopenTestMaster& master, std::uint32_t sequence,
                       std::uint32_t timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms);
    do {
        std::uint32_t active = 0U;
        std::uint32_t complete = 0U;
        if (!readControl(master, kActiveSeqSub, active)
            || !readControl(master, kCompleteSeqSub, complete)) {
            return false;
        }
        if (active == sequence && complete != sequence) {
            return true;
        }
        if (complete == sequence) {
            return false;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kPollIntervalMs));
    } while (std::chrono::steady_clock::now() < deadline);

    return false;
}

/** Submit one request and wait for its terminal snapshot. */
bool runTransaction(CanopenTestMaster& master, const ClientRequest& request,
                    ClientStatus& status, std::uint32_t& sequence)
{
    if (!nextRequestSequence(master, sequence)
        || !submitRequest(master, request, sequence)) {
        return false;
    }
    if (!waitSequence(master, kCompleteSeqSub, sequence,
                      kCompletionTimeoutMs)) {
        spdlog::error("MCU SDO client validation request {} completion timed out", sequence);
        return false;
    }
    if (!readStatus(master, status) || status.complete_seq != sequence
        || status.active_seq != sequence) {
        spdlog::error("MCU SDO client validation request {} returned inconsistent sequence state",
                      sequence);
        return false;
    }
    return true;
}

/** Check a successful U32 transaction result. */
bool expectU32Success(const char* case_id, const ClientStatus& status,
                      std::uint32_t expected_value)
{
    if (status.result != static_cast<std::int32_t>(McuSdoClientResult::SUCCESS)
        || status.abort_code != 0U || status.transferred_size != kU32Size
        || status.result_value != expected_value) {
        spdlog::error(
            "{} failed: result={} abort=0x{:08x} size={} value=0x{:08x}",
            case_id, status.result, status.abort_code,
            status.transferred_size, status.result_value);
        return false;
    }
    spdlog::info("{} passed", case_id);
    return true;
}

/** Check a specific abort/timeout terminal result. */
bool expectFailure(const char* case_id, const ClientStatus& status,
                   McuSdoClientResult expected_result,
                   std::uint32_t expected_abort)
{
    if (status.result != static_cast<std::int32_t>(expected_result)
        || status.abort_code != expected_abort) {
        spdlog::error("{} failed: result={} abort=0x{:08x}", case_id,
                      status.result, status.abort_code);
        return false;
    }
    spdlog::info("{} passed", case_id);
    return true;
}

/** Verify the timeout-test Node-ID has no SDO server response. */
bool verifyMissingNodePrecondition(CanopenTestMaster& master)
{
    std::uint32_t device_type = 0U;
    const SdoOperationResult result = readRemoteSdo<std::uint32_t>(
        master, kMissingNodeId, kDeviceTypeIndex, 0U, device_type,
        std::chrono::milliseconds(kMissingNodeProbeTimeoutMs),
        std::chrono::milliseconds(kMissingNodeProbeMarginMs));
    if (result == SdoOperationResult::SDO_TIMEOUT) {
        spdlog::info(
            "MCU SDO client validation timeout preflight confirmed node {} has no SDO response",
            static_cast<unsigned int>(kMissingNodeId));
        return true;
    }

    if (result == SdoOperationResult::SUCCESS) {
        spdlog::error(
            "MCU SDO client validation timeout preflight failed: node {} responded at 0x1000:00; "
            "choose an unused Node-ID",
            static_cast<unsigned int>(kMissingNodeId));
    } else {
        spdlog::error(
            "MCU SDO client validation timeout preflight was inconclusive for node {} (result={}); "
            "do not run timeout cases",
            static_cast<unsigned int>(kMissingNodeId),
            static_cast<int>(result));
    }
    return false;
}

/** Restore MCU 0x2200 independently through its Server-SDO and verify it. */
bool restoreLocalControl(CanopenTestMaster& master, std::uint32_t original)
{
    if (writeRemoteSdo<std::uint32_t>(
            master, CANOPEN_SLAVE_NODE_ID, kLocalControlIndex, 0U, original)
        != SdoOperationResult::SUCCESS) {
        spdlog::error("MCU SDO client validation cleanup failed restoring MCU 0x2200:00");
        return false;
    }
    std::uint32_t restored = 0U;
    if (readRemoteSdo<std::uint32_t>(
            master, CANOPEN_SLAVE_NODE_ID, kLocalControlIndex, 0U, restored)
            != SdoOperationResult::SUCCESS
        || restored != original) {
        spdlog::error("MCU SDO client validation cleanup could not verify MCU 0x2200:00");
        return false;
    }
    return true;
}

/** Restore Host fixture 0x2F00 and verify its baseline value. */
bool restoreFixtureU32(HostSdoServerFixture& fixture, std::uint32_t original)
{
    std::uint32_t restored = 0U;
    if (!fixture.writeU32(original) || !fixture.readU32(restored)
        || restored != original) {
        spdlog::error("MCU SDO client validation cleanup could not restore/verify Host 0x2F00:00");
        return false;
    }
    return true;
}

/** Restore Host fixture 0x2F01 and verify the complete baseline byte array. */
bool restoreFixtureOctets(HostSdoServerFixture& fixture,
                          const std::vector<std::uint8_t>& original)
{
    std::vector<std::uint8_t> restored;
    if (!fixture.writeOctets(original) || !fixture.readOctets(restored)
        || restored != original) {
        spdlog::error("MCU SDO client validation cleanup could not restore/verify Host 0x2F01:00");
        return false;
    }
    return true;
}

} // namespace

int sdoClientProcess(CanopenTestMaster& master)
{
    static_assert(kMissingNodeId != CANOPEN_MASTER_NODE_ID,
                  "MCU SDO client validation timeout node must not be the Host master");
    static_assert(kMissingNodeId != CANOPEN_SLAVE_NODE_ID,
                  "MCU SDO client validation timeout node must not be the MCU under test");

    if (!verifyMissingNodePrecondition(master)) {
        return 1;
    }

    HostSdoServerFixture fixture(master);
    if (!fixture.install()) {
        return 1;
    }

    ClientStatus status;
    std::uint32_t sequence = 0U;
    std::uint32_t original_local = 0U;
    if (readRemoteSdo<std::uint32_t>(
            master, CANOPEN_SLAVE_NODE_ID, kLocalControlIndex, 0U,
            original_local) != SdoOperationResult::SUCCESS) {
        return 1;
    }

    /* MCU SDO client local U32 upload: local U32 upload through CO_CONFIG_SDO_CLI_LOCAL. */
    ClientRequest request{kUploadCommand, CANOPEN_SLAVE_NODE_ID,
                          kLocalControlIndex, 0U, 0U, 0U, kNoFlags};
    if (!runTransaction(master, request, status, sequence)
        || !expectU32Success("MCU SDO client local U32 upload local U32 upload", status,
                             original_local)) {
        return 1;
    }

    /* MCU SDO client local download/readback: local download, independent Host read-back, and strict restore. */
    const std::uint32_t local_probe = original_local == kProbeValue
        ? kAlternateProbeValue : kProbeValue;
    request = {kDownloadCommand, CANOPEN_SLAVE_NODE_ID, kLocalControlIndex,
               0U, kU32Size, local_probe, kNoFlags};
    const bool local_transaction_ok =
        runTransaction(master, request, status, sequence);
    std::uint32_t local_readback = 0U;
    bool local_case_ok = local_transaction_ok
        && expectU32Success("MCU SDO client local download/readback local U32 download", status, local_probe);
    if (local_case_ok) {
        local_case_ok = readRemoteSdo<std::uint32_t>(
                            master, CANOPEN_SLAVE_NODE_ID,
                            kLocalControlIndex, 0U, local_readback)
                == SdoOperationResult::SUCCESS
            && local_readback == local_probe;
    }
    /* Restore even after a failed transaction because a partial write must not
     * leak into later cases. */
    if (!restoreLocalControl(master, original_local)) {
        return 1;
    }
    if (!local_case_ok) {
        spdlog::error("MCU SDO client local download/readback read-back mismatch");
        return 1;
    }

    /* MCU SDO client local abort recovery: local missing-object abort followed by a legal local upload. */
    request = {kUploadCommand, CANOPEN_SLAVE_NODE_ID, kMissingLocalIndex,
               0U, 0U, 0U, kNoFlags};
    if (!runTransaction(master, request, status, sequence)
        || !expectFailure("MCU SDO client local abort recovery local missing-object abort", status,
                          McuSdoClientResult::ABORT, kAbortNotExist)) {
        return 1;
    }
    request = {kUploadCommand, CANOPEN_SLAVE_NODE_ID, kLocalControlIndex,
               0U, 0U, 0U, kNoFlags};
    if (!runTransaction(master, request, status, sequence)
        || !expectU32Success("MCU SDO client local-abort recovery", status,
                             original_local)) {
        return 1;
    }

    /* MCU SDO client remote expedited upload: remote expedited upload from the Host Node-127 SSDO fixture. */
    std::uint32_t remote_u32 = 0U;
    if (!fixture.readU32(remote_u32)) {
        return 1;
    }
    request = {kUploadCommand, CANOPEN_MASTER_NODE_ID,
               HostSdoServerFixture::kU32Index,
               HostSdoServerFixture::kSubindex, 0U, 0U, kNoFlags};
    if (!runTransaction(master, request, status, sequence)
        || !expectU32Success("MCU SDO client remote expedited upload remote U32 upload", status,
                             remote_u32)) {
        return 1;
    }

    /* MCU SDO client remote expedited download: remote expedited download and direct local fixture read-back. */
    const std::uint32_t remote_probe = remote_u32 == kProbeValue
        ? kAlternateProbeValue : kProbeValue;
    request = {kDownloadCommand, CANOPEN_MASTER_NODE_ID,
               HostSdoServerFixture::kU32Index,
               HostSdoServerFixture::kSubindex, kU32Size, remote_probe,
               kNoFlags};
    const bool remote_transaction_ok =
        runTransaction(master, request, status, sequence);
    std::uint32_t remote_readback = 0U;
    const bool remote_case_ok = remote_transaction_ok
        && expectU32Success("MCU SDO client remote expedited download remote U32 download", status, remote_probe)
        && fixture.readU32(remote_readback) && remote_readback == remote_probe;
    /* Restore regardless of terminal result because the server may have seen
     * part or all of the download before the client reported failure. */
    if (!restoreFixtureU32(fixture, remote_u32)) {
        return 1;
    }
    if (!remote_case_ok) {
        spdlog::error("MCU SDO client remote expedited download fixture read-back mismatch");
        return 1;
    }

    /* MCU SDO client remote segmented upload: remote segmented upload of 48 bytes through the 32-byte FIFO. */
    const std::vector<std::uint8_t> segmented =
        makeSegmentedPayload(kSegmentedSeed);
    std::vector<std::uint8_t> original_octets;
    if (!fixture.readOctets(original_octets)) {
        return 1;
    }
    if (!fixture.writeOctets(segmented)) {
        /* co_sub_set_val() may replace a variable-length value before an
         * allocation error is reported, so still attempt baseline restore. */
        if (!restoreFixtureOctets(fixture, original_octets)) {
            spdlog::error("MCU SDO client remote segmented upload setup cleanup verification failed");
        }
        return 1;
    }
    request = {kUploadCommand, CANOPEN_MASTER_NODE_ID,
               HostSdoServerFixture::kOctetsIndex,
               HostSdoServerFixture::kSubindex, 0U, 0U, kNoFlags};
    const bool segmented_upload_transaction_ok =
        runTransaction(master, request, status, sequence);
    const bool segmented_upload_ok = segmented_upload_transaction_ok
        && status.result
            == static_cast<std::int32_t>(McuSdoClientResult::SUCCESS)
        && status.abort_code == 0U
        && status.transferred_size == kSegmentedSize
        && status.checksum == checksum(segmented);
    if (!restoreFixtureOctets(fixture, original_octets)) {
        return 1;
    }
    if (!segmented_upload_ok) {
        spdlog::error("MCU SDO client remote segmented upload remote segmented upload failed");
        return 1;
    }
    spdlog::info("MCU SDO client remote segmented upload remote segmented upload passed");

    /* MCU SDO client remote segmented download: remote segmented download, byte-for-byte verify, then restore. */
    if (!fixture.readOctets(original_octets)) {
        return 1;
    }
    request = {kDownloadCommand, CANOPEN_MASTER_NODE_ID,
               HostSdoServerFixture::kOctetsIndex,
               HostSdoServerFixture::kSubindex, kSegmentedSize,
               kSegmentedSeed, kNoFlags};
    const bool segmented_download_transaction_ok =
        runTransaction(master, request, status, sequence);
    std::vector<std::uint8_t> downloaded_octets;
    const bool segmented_download_ok = segmented_download_transaction_ok
        && status.result == static_cast<std::int32_t>(McuSdoClientResult::SUCCESS)
        && status.abort_code == 0U
        && status.transferred_size == kSegmentedSize
        && status.checksum == checksum(segmented)
        && fixture.readOctets(downloaded_octets)
        && downloaded_octets == segmented;
    if (!restoreFixtureOctets(fixture, original_octets)) {
        return 1;
    }
    if (!segmented_download_ok) {
        spdlog::error("MCU SDO client remote segmented download remote segmented download failed");
        return 1;
    }
    spdlog::info("MCU SDO client remote segmented download remote segmented download passed");

    /* MCU SDO client read-only abort recovery: remote read-only abort followed by a legal remote upload. */
    request = {kDownloadCommand, CANOPEN_MASTER_NODE_ID,
               HostSdoServerFixture::kReadOnlyIndex,
               HostSdoServerFixture::kSubindex, kU32Size, kProbeValue,
               kNoFlags};
    if (!runTransaction(master, request, status, sequence)
        || !expectFailure("MCU SDO client read-only abort recovery remote read-only abort", status,
                          McuSdoClientResult::ABORT, kAbortReadOnly)) {
        return 1;
    }
    request = {kUploadCommand, CANOPEN_MASTER_NODE_ID,
               HostSdoServerFixture::kU32Index,
               HostSdoServerFixture::kSubindex, 0U, 0U, kNoFlags};
    if (!runTransaction(master, request, status, sequence)
        || !expectU32Success("MCU SDO client read-only-abort recovery", status, remote_u32)) {
        return 1;
    }

    /* MCU SDO client missing-node timeout: absent server must terminate with the native timeout abort. */
    request = {kUploadCommand, kMissingNodeId,
               HostSdoServerFixture::kU32Index,
               HostSdoServerFixture::kSubindex, 0U, 0U, kNoFlags};
    if (!runTransaction(master, request, status, sequence)
        || !expectFailure("MCU SDO client missing-node timeout nonexistent-node timeout", status,
                          McuSdoClientResult::TIMEOUT, kAbortTimeout)) {
        return 1;
    }

    /* MCU SDO client timeout recovery: the same client instance must work after the timeout. */
    request = {kUploadCommand, CANOPEN_MASTER_NODE_ID,
               HostSdoServerFixture::kU32Index,
               HostSdoServerFixture::kSubindex, 0U, 0U, kNoFlags};
    if (!runTransaction(master, request, status, sequence)
        || !expectU32Success("MCU SDO client timeout recovery", status, remote_u32)) {
        return 1;
    }

    /* MCU SDO client reset cancellation/recovery: reset only after active_seq proves the timeout request started. */
    request = {kUploadCommand, kMissingNodeId,
               HostSdoServerFixture::kU32Index,
               HostSdoServerFixture::kSubindex, 0U, 0U, kNoFlags};
    if (!nextRequestSequence(master, sequence)
        || !submitRequest(master, request, sequence)
        || !waitActivePending(master, sequence, kActiveTimeoutMs)) {
        spdlog::error("MCU SDO client reset cancellation/recovery could not establish an active transaction");
        return 1;
    }

    prepareBootWait();
    if (!issueNmtCommand(master, lely::canopen::NmtCommand::RESET_COMM,
                         CANOPEN_SLAVE_NODE_ID,
                         "MCU SDO client validation Reset Communication cancellation")
        || !waitForBootCompletion(
            std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS))) {
        spdlog::error("MCU SDO client reset cancellation/recovery Reset Communication did not complete");
        return 1;
    }
    if (!waitSequence(master, kCompleteSeqSub, sequence,
                      kCompletionTimeoutMs)
        || !readStatus(master, status)
        || status.active_seq != sequence || status.complete_seq != sequence
        || status.result
            != static_cast<std::int32_t>(McuSdoClientResult::RESET_CANCELLED)
        || status.abort_code != 0U) {
        spdlog::error("MCU SDO client reset cancellation/recovery reset cancellation result mismatch");
        return 1;
    }

    request = {kUploadCommand, CANOPEN_MASTER_NODE_ID,
               HostSdoServerFixture::kU32Index,
               HostSdoServerFixture::kSubindex, 0U, 0U, kNoFlags};
    if (!runTransaction(master, request, status, sequence)
        || !expectU32Success("MCU SDO client reset-cancellation recovery", status, remote_u32)) {
        return 1;
    }

#if CANOPEN_ENABLE_SDO_CLIENT_BLOCK_REGRESSION
    /* MCU SDO client block-transfer regression: MCU block upload from the Host SSDO fixture. This gate is
     * intentionally separate from MCU SDO client integration validation so flags=0 behavior remains unchanged. */
    std::vector<std::uint8_t> block_original;
    const std::vector<std::uint8_t> block_payload =
        makePayload(kBlockRegressionSize, kBlockRegressionSeed);
    if (!fixture.readOctets(block_original)) {
        return 1;
    }
    if (!fixture.writeOctets(block_payload)) {
        if (!restoreFixtureOctets(fixture, block_original)) {
            spdlog::error("MCU SDO client block-transfer regression upload setup cleanup verification failed");
        }
        return 1;
    }
    request = {kUploadCommand, CANOPEN_MASTER_NODE_ID,
               HostSdoServerFixture::kOctetsIndex,
               HostSdoServerFixture::kSubindex, 0U, 0U, kBlockFlag};
    const bool block_upload_transaction_ok =
        runTransaction(master, request, status, sequence);
    const bool block_upload_ok = block_upload_transaction_ok
        && status.result == static_cast<std::int32_t>(McuSdoClientResult::SUCCESS)
        && status.abort_code == 0U
        && status.transferred_size == kBlockRegressionSize
        && status.checksum == checksum(block_payload);
    if (!restoreFixtureOctets(fixture, block_original)) {
        return 1;
    }
    if (!block_upload_ok) {
        spdlog::error("MCU SDO client block-transfer regression upload failed");
        return 1;
    }
    spdlog::info("MCU SDO client block-transfer regression upload passed");

    /* MCU SDO client block-transfer regression: MCU block download to the Host SSDO fixture and independent
     * byte-for-byte verification through the local object API. */
    if (!fixture.readOctets(block_original)) {
        return 1;
    }
    request = {kDownloadCommand, CANOPEN_MASTER_NODE_ID,
               HostSdoServerFixture::kOctetsIndex,
               HostSdoServerFixture::kSubindex, kBlockRegressionSize,
               kBlockRegressionSeed, kBlockFlag};
    const bool block_download_transaction_ok =
        runTransaction(master, request, status, sequence);
    std::vector<std::uint8_t> block_downloaded;
    const bool block_download_ok = block_download_transaction_ok
        && status.result == static_cast<std::int32_t>(McuSdoClientResult::SUCCESS)
        && status.abort_code == 0U
        && status.transferred_size == kBlockRegressionSize
        && status.checksum == checksum(block_payload)
        && fixture.readOctets(block_downloaded)
        && block_downloaded == block_payload;
    if (!restoreFixtureOctets(fixture, block_original)) {
        return 1;
    }
    if (!block_download_ok) {
        spdlog::error("MCU SDO client block-transfer regression download failed");
        return 1;
    }
    spdlog::info("MCU SDO client block-transfer regression download passed");

    /* Re-run one ordinary segmented upload after block mode to prove that the
     * original MCU SDO client integration validation path remains usable on the same SDO client instance. */
    if (!fixture.readOctets(block_original)
        || !fixture.writeOctets(segmented)) {
        if (!block_original.empty()
            && !restoreFixtureOctets(fixture, block_original)) {
            spdlog::error("MCU SDO client segmented-regression setup cleanup failed");
        }
        return 1;
    }
    request = {kUploadCommand, CANOPEN_MASTER_NODE_ID,
               HostSdoServerFixture::kOctetsIndex,
               HostSdoServerFixture::kSubindex, 0U, 0U, kNoFlags};
    const bool post_block_segmented_transaction_ok =
        runTransaction(master, request, status, sequence);
    const bool post_block_segmented_ok = post_block_segmented_transaction_ok
        && status.result == static_cast<std::int32_t>(McuSdoClientResult::SUCCESS)
        && status.abort_code == 0U
        && status.transferred_size == kSegmentedSize
        && status.checksum == checksum(segmented);
    if (!restoreFixtureOctets(fixture, block_original)) {
        return 1;
    }
    if (!post_block_segmented_ok) {
        spdlog::error("MCU SDO client post-block segmented-transfer regression failed");
        return 1;
    }
    spdlog::info("MCU SDO client post-block segmented-transfer regression passed");
#endif /* CANOPEN_ENABLE_SDO_CLIENT_BLOCK_REGRESSION */

    spdlog::info("MCU SDO client validation passed");
    return 0;
}
