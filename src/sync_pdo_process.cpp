/**
 * @file
 * @brief Implements SYNC consumer and synchronous TPDO validation.
 */

#include "sync_pdo_process.h"

#include "canopen_config.h"
#include "canopen_nmt.h"
#include "canopen_sdo.h"
#include "nmt_heartbeat.h"

#include <lely/coapp/master.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

namespace {

/** PDO number 1 is the synchronous TPDO under test. */
constexpr int kPdoNumber = 1;
/** OD 0x1005 contains the SYNC COB-ID and producer-role bit. */
constexpr std::uint16_t kSyncCobIdIndex = 0x1005;
/** OD 0x1006 contains the local SYNC communication cycle period in us. */
constexpr std::uint16_t kSyncPeriodIndex = 0x1006;
/** OD 0x1019 configures the optional SYNC counter overflow value. */
constexpr std::uint16_t kSyncCounterOverflowIndex = 0x1019;
/** OD 0x1800 contains TPDO1 communication parameters. */
constexpr std::uint16_t kTpdoCommIndex = 0x1800;
/** Scalar/common sub-index used by the SYNC objects. */
constexpr std::uint8_t kSubindex0 = 0x00;
/** TPDO1 COB-ID sub-index in 0x1800. */
constexpr std::uint8_t kTpdoCobIdSubindex = 0x01;
/** TPDO1 transmission type sub-index in 0x1800. */
constexpr std::uint8_t kTpdoTransmissionTypeSubindex = 0x02;
/** TPDO1 inhibit time sub-index in 0x1800. */
constexpr std::uint8_t kTpdoInhibitTimeSubindex = 0x03;
/** TPDO1 event timer sub-index in 0x1800. */
constexpr std::uint8_t kTpdoEventTimerSubindex = 0x05;
/** TPDO1 SYNC start value sub-index in 0x1800. */
constexpr std::uint8_t kTpdoSyncStartSubindex = 0x06;

/** Bit 30 of 0x1005 selects SYNC producer capability at runtime. */
constexpr std::uint32_t kSyncProducerMask = 0x40000000U;
/** Bit 31 of a PDO COB-ID disables that PDO while communication parameters are changed. */
constexpr std::uint32_t kTpdoInvalidMask = 0x80000000U;
/** Transmission type 1 means one synchronous TPDO for every received SYNC. */
constexpr std::uint8_t kSynchronousTransmissionType = 1U;
/** Types 254 and 255 are asynchronous/event-driven and therefore valid A04 baselines. */
constexpr std::uint8_t kEventTransmissionTypeMinimum = 254U;
/** 200 ms gives clear host-side timing separation without making the stage slow. */
constexpr std::uint32_t kSyncPeriodUs = 200000U;
/** Millisecond form of the same period for host waits and interval bounds. */
constexpr std::uint32_t kSyncPeriodMs = kSyncPeriodUs / 1000U;
/** Five SYNC events provide enough pairs to detect missing/duplicate TPDO behavior. */
constexpr std::size_t kSyncSampleCount = 5U;
/** Two event-driven TPDO frames are sufficient to verify one restored timer interval. */
constexpr std::size_t kEventSampleCount = 2U;
/** TPDO1 maps two UNSIGNED32 values and therefore carries eight bytes. */
constexpr std::size_t kTpdoPayloadLength = 8U;
/** Two spare slots allow post-stop/overflow detection beyond the five expected pairs. */
constexpr std::size_t kObservationCapacity = kSyncSampleCount + 2U;
/** Minimum no-SYNC observation window is longer than the original 1 s event timer. */
constexpr std::uint32_t kMinimumQuietWindowMs = 1500U;
/** Restored event-timer tolerance matches the A03 host-side timing allowance. */
constexpr std::uint32_t kEventTimerToleranceMs = 150U;
/** Extra collection margin allows the first restored event TPDO to start at any phase. */
constexpr std::uint32_t kEventCollectionMarginMs = 1500U;

/** Monotonic clock used to compare callback order and latency. */
using Clock = std::chrono::steady_clock;
/** Lely PDO callback type retained so A04 can unregister its callback explicitly. */
using PdoCallback =
    std::function<void(int, std::error_code, const void*, std::size_t)>;
/** Lely SYNC callback type retained for explicit callback cleanup. */
using SyncCallback = std::function<void(
    std::uint8_t, const lely::canopen::AsyncMaster::time_point&)>;
/** Lely SYNC error callback type retained for explicit callback cleanup. */
using SyncErrorCallback = std::function<void(std::uint16_t, std::uint8_t)>;

/** Complete TPDO1 communication-parameter snapshot required for strict restore verification. */
struct TpdoCommSnapshot {
    std::uint8_t highest_subindex = 0; /**< Zero until 0x1800:00 is uploaded. */
    std::uint32_t cob_id = 0; /**< Zero placeholder for original 0x1800:01. */
    std::uint8_t transmission_type = 0; /**< Zero placeholder for original 0x1800:02. */
    std::uint16_t inhibit_time = 0; /**< Zero placeholder for original 0x1800:03. */
    std::uint16_t event_timer = 0; /**< Zero placeholder for original 0x1800:05. */
    std::uint8_t sync_start = 0; /**< Zero placeholder for original 0x1800:06. */
};

/** Callback-owned observation state for SYNC and TPDO timing assertions. */
struct SyncPdoObservation {
    std::mutex mutex; /**< Protects all callback-published observation fields. */
    std::condition_variable condition; /**< Wakes process waits on SYNC/TPDO activity. */
    std::array<Clock::time_point, kObservationCapacity> sync_timestamps{}; /**< Zero-initialized SYNC callback times. */
    std::array<Clock::time_point, kObservationCapacity> tpdo_timestamps{}; /**< Zero-initialized TPDO callback times. */
    std::size_t sync_count = 0; /**< Number of SYNC callbacks observed since reset. */
    std::size_t tpdo_count = 0; /**< Number of TPDO1 callbacks observed since reset. */
    bool failed = false; /**< false until any callback-level invariant fails. */
    std::error_code tpdo_error; /**< Default success; stores Lely TPDO processing error. */
    std::error_code sync_stop_error; /**< Default success; stores error stopping local SYNC. */
    std::size_t invalid_length = 0; /**< Zero before failure; stores callback-reported DLC on invalid TPDO1 input. */
    bool invalid_tpdo = false; /**< Marks invalid TPDO callback metadata or payload. */
    bool null_payload = false; /**< Marks nonzero TPDO length with null payload pointer. */
    bool sync_error = false; /**< Marks an OnSyncError callback. */
    std::uint16_t sync_error_code = 0; /**< Zero until SYNC error callback supplies an EMCY code. */
    std::uint8_t sync_error_register = 0; /**< Zero until SYNC error callback supplies error register. */
};

/**
 * @brief Read the TPDO1 communication parameters that A04 must preserve.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param snapshot Receives the original TPDO1 parameters.
 * @param completion_wait_timed_out Set if a local completion wait times out.
 * @return true when every parameter was read successfully; otherwise false.
 */
bool readTpdoSnapshot(lely::canopen::AsyncMaster& master,
                      TpdoCommSnapshot& snapshot,
                      bool& completion_wait_timed_out)
{
    /* Generic local lambda keeps identical WAIT_TIMEOUT propagation for every
     * TPDO1 communication sub-index without duplicating six error branches. */
    const auto read = [&master, &completion_wait_timed_out](
                          std::uint8_t subindex, auto& value) {
        /* Preserve the exact remote SDO result for safety classification. */
        const SdoOperationResult result =
            readRemoteSdo(master, CANOPEN_SLAVE_NODE_ID, kTpdoCommIndex,
                          subindex, value);
        if (result == SdoOperationResult::WAIT_TIMEOUT) {
            completion_wait_timed_out = true;
        }
        return result == SdoOperationResult::SUCCESS;
    };

    return read(kSubindex0, snapshot.highest_subindex)
           && read(kTpdoCobIdSubindex, snapshot.cob_id)
           && read(kTpdoTransmissionTypeSubindex,
                   snapshot.transmission_type)
           && read(kTpdoInhibitTimeSubindex, snapshot.inhibit_time)
           && read(kTpdoEventTimerSubindex, snapshot.event_timer)
           && read(kTpdoSyncStartSubindex, snapshot.sync_start);
}

/**
 * @brief Compare a TPDO1 parameter snapshot with the expected values.
 *
 * @param expected Saved original parameters.
 * @param actual Parameters read after restoration.
 * @return true when every saved parameter matches; otherwise false.
 */
bool compareTpdoSnapshot(const TpdoCommSnapshot& expected,
                         const TpdoCommSnapshot& actual)
{
    /* Start optimistic and keep checking every field so one run reports all
     * restoration mismatches instead of only the first one. */
    bool matches = true;
    /* Compare fields through one diagnostic path while keeping their names in
     * the error log. */
    const auto mismatch = [&matches](const char* name, std::uint32_t expected_value,
                                     std::uint32_t actual_value) {
        if (expected_value != actual_value) {
            spdlog::error(
                "A04 restored TPDO1 {} mismatch: expected=0x{:x} actual=0x{:x}",
                name, expected_value, actual_value);
            matches = false;
        }
    };

    mismatch("sub0", expected.highest_subindex, actual.highest_subindex);
    mismatch("COB-ID", expected.cob_id, actual.cob_id);
    mismatch("transmission type", expected.transmission_type,
             actual.transmission_type);
    mismatch("inhibit time", expected.inhibit_time, actual.inhibit_time);
    mismatch("event timer", expected.event_timer, actual.event_timer);
    mismatch("SYNC start", expected.sync_start, actual.sync_start);
    return matches;
}

/**
 * @brief Verify the restored TPDO1 snapshot and slave SYNC COB-ID.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param original Saved TPDO1 communication parameters.
 * @param expected_sync_cob_id Saved slave 0x1005:00 value.
 * @param completion_wait_timed_out Set if a local SDO completion wait times out.
 * @return true when all restored values match; otherwise false.
 */
bool verifyRestoredTpdoState(lely::canopen::AsyncMaster& master,
                             const TpdoCommSnapshot& original,
                             std::uint32_t expected_sync_cob_id,
                             bool& completion_wait_timed_out)
{
    /* Value-initialized snapshot is overwritten only if all six SDO uploads
     * succeed, avoiding use of partially initialized restoration data. */
    TpdoCommSnapshot restored{};
    if (!readTpdoSnapshot(master, restored, completion_wait_timed_out)) {
        spdlog::error("A04 TPDO1 restoration read-back failed");
        return false;
    }
    if (!compareTpdoSnapshot(original, restored)) {
        return false;
    }

    /* Zero is a neutral placeholder until the remote 0x1005 upload succeeds. */
    std::uint32_t restored_sync_cob_id = 0;
    /* Preserve completion classification because WAIT_TIMEOUT forbids further
     * SDO assumptions until a communication reset boundary is established. */
    const SdoOperationResult sync_read_result = readRemoteSdo(
        master, CANOPEN_SLAVE_NODE_ID, kSyncCobIdIndex, kSubindex0,
        restored_sync_cob_id);
    if (sync_read_result == SdoOperationResult::WAIT_TIMEOUT) {
        completion_wait_timed_out = true;
        return false;
    }
    if (sync_read_result != SdoOperationResult::SUCCESS) {
        return false;
    }
    if (restored_sync_cob_id != expected_sync_cob_id) {
        spdlog::error(
            "A04 slave SYNC COB-ID changed during test: "
            "expected=0x{:08x} actual=0x{:08x}",
            expected_sync_cob_id, restored_sync_cob_id);
        return false;
    }

    spdlog::info(
        "A04 TPDO1 communication parameters restored and verified");
    return true;
}

/**
 * @brief Recover TPDO1 whenever normal restoration cannot be verified.
 *
 * Reset Communication establishes a clean protocol/configuration boundary
 * after an unknown SDO completion, restore write failure, NMT transition
 * failure, or restoration read-back mismatch. The existing Lely Boot
 * configuration is then allowed to restore the node before the saved values
 * are read back.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param original Saved TPDO1 communication parameters.
 * @param expected_sync_cob_id Saved slave 0x1005:00 value.
 * @param completion_wait_timed_out Updated if recovery SDO completion times out.
 * @param slave_needs_start Set once reset is issued so cleanup restores NMT Start.
 * @return true when Boot completes and restored values match; otherwise false.
 */
bool recoverTpdoStateWithReset(lely::canopen::AsyncMaster& master,
                               const TpdoCommSnapshot& original,
                               std::uint32_t expected_sync_cob_id,
                               bool& completion_wait_timed_out,
                               bool& slave_needs_start)
{
    spdlog::warn(
        "A04 recovering unverified TPDO1 state with slave Reset Communication");

    /* Step 1: clear stale Boot state before Reset Communication can complete. */
    prepareBootWait();
    /* Step 2: Reset Communication aborts/reinitializes the uncertain SDO
     * transaction without requiring a full application reset. */
    if (!issueNmtCommand(master, lely::canopen::NmtCommand::RESET_COMM,
                         CANOPEN_SLAVE_NODE_ID,
                         "A04 cleanup slave Reset Communication")) {
        return false;
    }
    /* From this point cleanup owns the slave NMT state. Lely Boot management
     * may auto-start a mandatory slave, so recovery reasserts Pre-operational
     * after Boot before any restoration verification. */
    slave_needs_start = true;

    /* Step 3: a fresh Boot callback proves the communication reset completed. */
    if (!waitForBootCompletion(
            std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS))) {
        spdlog::error(
            "A04 slave did not complete Boot during TPDO1 recovery");
        return false;
    }

    /* Step 4: Lely may have auto-started a mandatory slave during Boot. Force
     * Pre-operational again and require a fresh remote state indication before
     * recovery performs its final read-back. */
    if (!issueNmtCommandAndWaitForState(
            master, lely::canopen::NmtCommand::ENTER_PREOP,
            CANOPEN_SLAVE_NODE_ID, lely::canopen::NmtState::PREOP,
            std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS),
            "A04 recovery slave NMT Enter Pre-operational")) {
        return false;
    }

    /* Step 5: the completed reset creates a new transaction boundary, so the
     * previous unknown callback state no longer blocks a fresh verification. */
    completion_wait_timed_out = false;
    if (!verifyRestoredTpdoState(master, original, expected_sync_cob_id,
                                 completion_wait_timed_out)) {
        spdlog::error(
            "A04 Reset Communication recovery did not restore TPDO1");
        return false;
    }

    spdlog::info(
        "A04 Reset Communication recovered and verified TPDO1 state");
    return true;
}

/**
 * @brief Read and validate the SYNC producer/consumer topology.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param remote_sync_cob_id Receives slave 0x1005:00.
 * @param local_sync_period Receives original local 0x1006:00.
 * @param local_sync_period_saved Set after local 0x1006:00 is read.
 * @param completion_wait_timed_out Set if the remote SDO completion is unknown.
 * @return true when the master is the sole configured SYNC producer and both
 * nodes use compatible SYNC frame formats; otherwise false.
 */
bool validateSyncTopology(lely::canopen::AsyncMaster& master,
                          std::uint32_t& remote_sync_cob_id,
                          std::uint32_t& local_sync_period,
                          bool& local_sync_period_saved,
                          bool& completion_wait_timed_out)
{
    /* Zero represents counter-less SYNC until the remote 0x1019 upload proves
     * otherwise. */
    std::uint8_t remote_sync_overflow = 0;
    /* Reuse one result variable because the two topology SDO reads are strictly
     * sequential and each is handled before the next starts. */
    SdoOperationResult result = readRemoteSdo(
        master, CANOPEN_SLAVE_NODE_ID, kSyncCobIdIndex, kSubindex0,
        remote_sync_cob_id);
    if (result == SdoOperationResult::WAIT_TIMEOUT) {
        completion_wait_timed_out = true;
        return false;
    }
    if (result != SdoOperationResult::SUCCESS) {
        return false;
    }

    result = readRemoteSdo(master, CANOPEN_SLAVE_NODE_ID,
                           kSyncCounterOverflowIndex, kSubindex0,
                           remote_sync_overflow);
    if (result == SdoOperationResult::WAIT_TIMEOUT) {
        completion_wait_timed_out = true;
        return false;
    }
    if (result != SdoOperationResult::SUCCESS) {
        return false;
    }

    /* Local OD reads are synchronous and do not issue SDO frames; error starts
     * clear and is populated directly by each Lely Read(). */
    std::error_code error;
    /* 0x1005 local read establishes that the Linux master, not the slave, is
     * the configured SYNC producer for this stage. */
    const std::uint32_t local_sync_cob_id =
        master.Read<std::uint32_t>(kSyncCobIdIndex, kSubindex0, error);
    if (error) {
        spdlog::error("A04 unable to read local 0x1005:00: {}",
                      error.message());
        return false;
    }
    /* Save the original local 0x1006 so A04 can restore pre-test producer
     * behavior even if the original period was nonzero. */
    local_sync_period =
        master.Read<std::uint32_t>(kSyncPeriodIndex, kSubindex0, error);
    if (error) {
        spdlog::error("A04 unable to read local 0x1006:00: {}",
                      error.message());
        return false;
    }
    /* Mark saved only after a successful read; cleanup must not invent a value. */
    local_sync_period_saved = true;
    /* Counter format must match the slave; zero is required by this type-1
     * test so each SYNC frame is counter-less and directly comparable. */
    const std::uint8_t local_sync_overflow =
        master.Read<std::uint8_t>(kSyncCounterOverflowIndex, kSubindex0,
                                  error);
    if (error) {
        spdlog::error("A04 unable to read local 0x1019:00: {}",
                      error.message());
        return false;
    }

    if ((remote_sync_cob_id & kSyncProducerMask) != 0U) {
        spdlog::error(
            "A04 slave 0x1005:00=0x{:08x} enables SYNC producer; the test "
            "requires node {} to remain a SYNC consumer",
            remote_sync_cob_id, CANOPEN_SLAVE_NODE_ID);
        return false;
    }
    if ((local_sync_cob_id & kSyncProducerMask) == 0U) {
        spdlog::error(
            "A04 local 0x1005:00=0x{:08x} does not enable the master SYNC "
            "producer",
            local_sync_cob_id);
        return false;
    }
    if ((remote_sync_cob_id & ~kSyncProducerMask)
        != (local_sync_cob_id & ~kSyncProducerMask)) {
        spdlog::error(
            "A04 SYNC COB-ID mismatch: slave=0x{:08x} master=0x{:08x}",
            remote_sync_cob_id, local_sync_cob_id);
        return false;
    }
    if (remote_sync_overflow != local_sync_overflow) {
        spdlog::error(
            "A04 SYNC counter format mismatch: slave_0x1019={} "
            "master_0x1019={}",
            static_cast<unsigned int>(remote_sync_overflow),
            static_cast<unsigned int>(local_sync_overflow));
        return false;
    }
    if (local_sync_overflow != 0U) {
        spdlog::error(
            "A04 requires counter-less SYNC for transmission type 1 test: "
            "0x1019={}",
            static_cast<unsigned int>(local_sync_overflow));
        return false;
    }

    spdlog::info(
        "A04 SYNC topology verified: master producer 0x{:08x}, slave "
        "consumer 0x{:08x}, original period={} us",
        local_sync_cob_id, remote_sync_cob_id, local_sync_period);
    return true;
}

/**
 * @brief Write the local Lely SYNC producer period.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param period_us Communication cycle period in microseconds.
 * @return true on success; otherwise false.
 */
bool writeLocalSyncPeriod(lely::canopen::AsyncMaster& master,
                          std::uint32_t period_us)
{
    /* Local OD Write controls Lely's own SYNC producer directly and therefore
     * does not create a remote SDO transaction. */
    std::error_code error;
    master.Write<std::uint32_t>(kSyncPeriodIndex, kSubindex0, period_us,
                                error);
    if (error) {
        spdlog::error("A04 unable to write local 0x1006:00={} us: {}",
                      period_us, error.message());
        return false;
    }
    return true;
}

/**
 * @brief Put TPDO1 into synchronous cyclic transmission type 1.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param original_cob_id Saved valid TPDO1 COB-ID.
 * @param modification_attempted Set before the first TPDO1-modifying SDO is
 *        attempted because a missing completion callback cannot prove that the
 *        slave rejected the write.
 * @param completion_wait_timed_out Set if the local SDO completion is unknown.
 * @return true when disable/type/enable writes all succeed; otherwise false.
 */
bool configureSynchronousTpdo(lely::canopen::AsyncMaster& master,
                              std::uint32_t original_cob_id,
                              bool& modification_attempted,
                              bool& completion_wait_timed_out)
{
    /* Reuse one typed lambda so all three legal PDO-configuration writes have
     * identical WAIT_TIMEOUT handling. */
    const auto write = [&master, &completion_wait_timed_out](
                           std::uint8_t subindex, auto value) {
        /* Each call is a remote SDO download to the slave TPDO1 parameters. */
        const SdoOperationResult result =
            writeRemoteSdo(master, CANOPEN_SLAVE_NODE_ID, kTpdoCommIndex,
                           subindex, value);
        if (result == SdoOperationResult::WAIT_TIMEOUT) {
            completion_wait_timed_out = true;
        }
        return result == SdoOperationResult::SUCCESS;
    };

    /* Step 1: once the first modifying SDO is attempted, cleanup must assume
     * the slave may have accepted it. The common helper intentionally merges
     * immediate submission and non-timeout completion failures into FAILED, while
     * WAIT_TIMEOUT explicitly leaves the remote state unknown. Mark the state
     * before calling it so neither case can be mistaken for "not modified". */
    modification_attempted = true;
    if (!write(kTpdoCobIdSubindex, original_cob_id | kTpdoInvalidMask)) {
        return false;
    }
    /* Step 2: transmission type 1 requests exactly one TPDO per SYNC. */
    if (!write(kTpdoTransmissionTypeSubindex,
               kSynchronousTransmissionType)) {
        return false;
    }
    /* Step 3: re-enable TPDO1 with the original COB-ID after the type change. */
    if (!write(kTpdoCobIdSubindex, original_cob_id)) {
        return false;
    }

    return true;
}

/**
 * @brief Restore the TPDO1 fields modified by A04.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param original Saved TPDO1 communication parameters.
 * @param completion_wait_timed_out Set if the local SDO completion is unknown.
 * @return true when disable/type/enable restoration succeeds; otherwise false.
 */
bool restoreTpdoConfiguration(lely::canopen::AsyncMaster& master,
                              const TpdoCommSnapshot& original,
                              bool& completion_wait_timed_out)
{
    /* Mirror the legal disable/change/enable sequence used for temporary
     * configuration so restoration does not modify an active TPDO in place. */
    const auto write = [&master, &completion_wait_timed_out](
                           std::uint8_t subindex, auto value) {
        /* Preserve WAIT_TIMEOUT because it changes the recovery strategy. */
        const SdoOperationResult result =
            writeRemoteSdo(master, CANOPEN_SLAVE_NODE_ID, kTpdoCommIndex,
                           subindex, value);
        if (result == SdoOperationResult::WAIT_TIMEOUT) {
            completion_wait_timed_out = true;
        }
        return result == SdoOperationResult::SUCCESS;
    };

    /* Step 1: disable TPDO1 using the saved original COB-ID. */
    if (!write(kTpdoCobIdSubindex, original.cob_id | kTpdoInvalidMask)) {
        return false;
    }
    /* Step 2: restore the exact original transmission type. */
    if (!write(kTpdoTransmissionTypeSubindex,
               original.transmission_type)) {
        return false;
    }
    /* Step 3: re-enable TPDO1 with its original COB-ID. */
    if (!write(kTpdoCobIdSubindex, original.cob_id)) {
        return false;
    }

    return true;
}

/**
 * @brief Reset all transient SYNC/TPDO observations.
 *
 * @param state Shared callback state.
 */
void resetObservation(const std::shared_ptr<SyncPdoObservation>& state)
{
    /* Reset all fields under one lock so callbacks cannot observe a mixed
     * generation between quiet-window, synchronous, and event-timer phases. */
    std::lock_guard<std::mutex> lock(state->mutex);
    state->sync_count = 0;
    state->tpdo_count = 0;
    state->failed = false;
    state->tpdo_error.clear();
    state->sync_stop_error.clear();
    state->invalid_length = 0;
    state->invalid_tpdo = false;
    state->null_payload = false;
    state->sync_error = false;
    state->sync_error_code = 0;
    state->sync_error_register = 0;
}

/**
 * @brief Install A04 SYNC and TPDO1 callbacks.
 *
 * The fifth generated SYNC stops the local periodic producer immediately so
 * the test has a deterministic sample count.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param state Shared callback state.
 */
void registerCallbacks(lely::canopen::AsyncMaster& master,
                       const std::shared_ptr<SyncPdoObservation>& state)
{
    master.OnRpdo(
        [state](int num, std::error_code error, const void* payload,
                std::size_t length) noexcept {
            if (num != kPdoNumber) {
                return;
            }

            /* Capture arrival time before taking the shared-state mutex so the
             * timestamp reflects callback entry rather than lock contention. */
            const Clock::time_point timestamp = Clock::now();
            {
                /* Publish TPDO validation result and timestamp atomically. */
                std::lock_guard<std::mutex> lock(state->mutex);
                if (error || length != kTpdoPayloadLength
                    || (payload == nullptr && length != 0U)) {
                    state->failed = true;
                    state->tpdo_error = error;
                    state->invalid_length = length;
                    state->invalid_tpdo = true;
                    state->null_payload =
                        payload == nullptr && length != 0U;
                } else {
                    if (state->tpdo_count
                        < state->tpdo_timestamps.size()) {
                        state->tpdo_timestamps[state->tpdo_count] = timestamp;
                    }
                    ++state->tpdo_count;
                }
            }
            state->condition.notify_all();
        });

    master.OnSync(
        [&master, state](
            std::uint8_t,
            const lely::canopen::AsyncMaster::time_point&) noexcept {
            /* Use the same host monotonic clock as TPDO callbacks so ordering
             * and latency comparisons share one time domain. */
            const Clock::time_point timestamp = Clock::now();
            /* false means continue producing SYNC; it becomes true exactly at
             * the fifth observed SYNC to make the sample count deterministic. */
            bool stop_sync = false;
            {
                /* Update SYNC count/timestamp and stop decision coherently. */
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->sync_count < state->sync_timestamps.size()) {
                    state->sync_timestamps[state->sync_count] = timestamp;
                }
                ++state->sync_count;
                stop_sync = state->sync_count == kSyncSampleCount;
            }

            if (stop_sync) {
                /* Zero disables the local periodic producer immediately after
                 * the fifth callback; error starts clear for this local write. */
                std::error_code error;
                master.Write<std::uint32_t>(kSyncPeriodIndex, kSubindex0,
                                            static_cast<std::uint32_t>(0),
                                            error);
                if (error) {
                    /* Publish failure under the same observation mutex used by
                     * process-side validation. */
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->failed = true;
                    state->sync_stop_error = error;
                }
            }
            state->condition.notify_all();
        });

    master.OnSyncError(
        [state](std::uint16_t error_code,
                std::uint8_t error_register) noexcept {
            {
                /* Any SYNC processing error invalidates the A04 timing sample. */
                std::lock_guard<std::mutex> lock(state->mutex);
                state->failed = true;
                state->sync_error = true;
                state->sync_error_code = error_code;
                state->sync_error_register = error_register;
            }
            state->condition.notify_all();
        });
}

/**
 * @brief Remove callbacks installed by A04.
 *
 * @param master Active Lely asynchronous CANopen master.
 */
void clearCallbacks(lely::canopen::AsyncMaster& master)
{
    master.OnRpdo(PdoCallback{});
    master.OnSync(SyncCallback{});
    master.OnSyncError(SyncErrorCallback{});
}

/**
 * @brief Report a callback-side A04 failure.
 *
 * @param state Shared callback state.
 * @return true when no callback failure is present; otherwise false.
 */
bool validateCallbackState(const std::shared_ptr<SyncPdoObservation>& state)
{
    /* Read all callback failure fields as one coherent snapshot. */
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->failed) {
        return true;
    }
    if (state->tpdo_error) {
        spdlog::error("A04 TPDO1 processing failed: {}",
                      state->tpdo_error.message());
    } else if (state->null_payload) {
        spdlog::error("A04 TPDO1 callback returned a null payload");
    } else if (state->invalid_tpdo) {
        spdlog::error("A04 TPDO1 length mismatch: expected={} actual={}",
                      kTpdoPayloadLength, state->invalid_length);
    } else if (state->sync_error) {
        spdlog::error(
            "A04 SYNC processing error: code=0x{:04x} register=0x{:02x}",
            state->sync_error_code,
            static_cast<unsigned int>(state->sync_error_register));
    } else if (state->sync_stop_error) {
        spdlog::error("A04 unable to stop SYNC producer after sample set: {}",
                      state->sync_stop_error.message());
    } else {
        spdlog::error("A04 callback state failed for an unknown reason");
    }
    return false;
}

/**
 * @brief Verify that no SYNC or TPDO1 occurs in the quiet window.
 *
 * @param state Shared callback state.
 * @param quiet_window_ms Observation duration.
 * @return true when the window remains silent; otherwise false.
 */
bool verifyQuietWindow(const std::shared_ptr<SyncPdoObservation>& state,
                       std::uint32_t quiet_window_ms)
{
    /* unique_lock allows callbacks to acquire the mutex during the timed wait. */
    std::unique_lock<std::mutex> lock(state->mutex);
    /* false at timeout means the full window stayed silent; true means either
     * traffic or callback failure occurred and must be diagnosed. */
    const bool activity = state->condition.wait_for(
        lock, std::chrono::milliseconds(quiet_window_ms), [state]() {
            return state->failed || state->sync_count != 0U
                   || state->tpdo_count != 0U;
        });
    if (activity) {
        if (state->failed) {
            lock.unlock();
            return validateCallbackState(state);
        }
        spdlog::error(
            "A04 quiet window violated: unexpected_sync={} unexpected_tpdo={}",
            state->sync_count, state->tpdo_count);
        return false;
    }

    spdlog::info("A04 quiet window passed: {} ms without SYNC or TPDO1",
                 quiet_window_ms);
    return true;
}

/**
 * @brief Wait for the requested synchronous sample set.
 *
 * @param state Shared callback state.
 * @return true when five SYNC and five TPDO1 indications are observed.
 */
bool waitForSynchronousSamples(
    const std::shared_ptr<SyncPdoObservation>& state)
{
    /* Two extra periods tolerate initial timer phase/scheduling jitter while
     * still bounding a missing-SYNC or missing-TPDO failure. */
    const std::uint32_t timeout_ms =
        (static_cast<std::uint32_t>(kSyncSampleCount) + 2U) * kSyncPeriodMs;
    /* Release the mutex while waiting so callbacks can increment both counts. */
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(
            lock, std::chrono::milliseconds(timeout_ms), [state]() {
                return state->failed
                       || (state->sync_count >= kSyncSampleCount
                           && state->tpdo_count >= kSyncSampleCount);
            })) {
        spdlog::error(
            "A04 synchronous sample wait timed out: sync={}/{} tpdo={}/{}",
            state->sync_count, kSyncSampleCount, state->tpdo_count,
            kSyncSampleCount);
        return false;
    }
    if (state->failed) {
        lock.unlock();
        return validateCallbackState(state);
    }
    return true;
}

/**
 * @brief Validate one TPDO1 inside each SYNC interval.
 *
 * @param state Shared callback state.
 * @return true when count and ordering constraints are met; otherwise false.
 */
bool validateSynchronousTiming(
    const std::shared_ptr<SyncPdoObservation>& state)
{
    /* Local SYNC timestamps are immediately overwritten by the coherent shared
     * snapshot below, so no separate value initialization is required. */
    std::array<Clock::time_point, kObservationCapacity> sync_timestamps;
    /* Local TPDO timestamps follow the same overwrite-before-use rule. */
    std::array<Clock::time_point, kObservationCapacity> tpdo_timestamps;
    /* Zero is the safe pre-snapshot count and is replaced while holding mutex. */
    std::size_t sync_count = 0;
    /* Zero likewise means no TPDO count has yet been copied from shared state. */
    std::size_t tpdo_count = 0;
    {
        /* Snapshot both arrays and counts atomically. */
        std::lock_guard<std::mutex> lock(state->mutex);
        sync_timestamps = state->sync_timestamps;
        tpdo_timestamps = state->tpdo_timestamps;
        sync_count = state->sync_count;
        tpdo_count = state->tpdo_count;
    }

    if (sync_count != kSyncSampleCount || tpdo_count != kSyncSampleCount) {
        spdlog::error(
            "A04 synchronous count mismatch: expected={} sync={} tpdo={}",
            kSyncSampleCount, sync_count, tpdo_count);
        return false;
    }

    /* i pairs SYNC[i] with TPDO[i] and defines that pair's timing window. */
    for (std::size_t i = 0; i < kSyncSampleCount; ++i) {
        if (tpdo_timestamps[i] < sync_timestamps[i]) {
            spdlog::error("A04 TPDO1[{}] arrived before SYNC[{}]", i, i);
            return false;
        }

        /* For pairs 0..3 the next SYNC is the exclusive deadline; the last
         * pair uses one configured SYNC period because no sixth SYNC exists. */
        const Clock::time_point deadline =
            i + 1U < kSyncSampleCount
                ? sync_timestamps[i + 1U]
                : sync_timestamps[i]
                      + std::chrono::milliseconds(kSyncPeriodMs);
        if (tpdo_timestamps[i] >= deadline) {
            spdlog::error(
                "A04 TPDO1[{}] crossed its SYNC interval boundary", i);
            return false;
        }

        /* Latency is diagnostic evidence, not a fixed pass/fail threshold;
         * the hard invariant is containment within the corresponding window. */
        const auto latency =
            std::chrono::duration_cast<std::chrono::microseconds>(
                tpdo_timestamps[i] - sync_timestamps[i]);
        spdlog::info("A04 SYNC/TPDO1 pair[{}] latency={} us", i,
                     latency.count());
    }

    return true;
}

/**
 * @brief Verify that stopping SYNC does not leave extra synchronous TPDO1.
 *
 * @param state Shared callback state.
 * @return true when counts stay fixed for one additional SYNC period.
 */
bool verifyPostSyncSilence(const std::shared_ptr<SyncPdoObservation>& state)
{
    /* One additional SYNC period is enough to detect a periodic producer that
     * failed to stop after the fifth sample. */
    std::this_thread::sleep_for(std::chrono::milliseconds(kSyncPeriodMs));
    if (!validateCallbackState(state)) {
        return false;
    }

    /* Counts must remain exactly five after the post-stop observation delay. */
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->sync_count != kSyncSampleCount
        || state->tpdo_count != kSyncSampleCount) {
        spdlog::error(
            "A04 extra traffic after SYNC stop: sync={} tpdo={} expected={}",
            state->sync_count, state->tpdo_count, kSyncSampleCount);
        return false;
    }
    return true;
}

/**
 * @brief Verify the restored event-driven TPDO1 timer.
 *
 * @param state Shared callback state.
 * @param event_timer_ms Original event timer in milliseconds.
 * @return true when two TPDO1 frames are received with the expected interval.
 */
bool verifyRestoredEventTpdo(
    const std::shared_ptr<SyncPdoObservation>& state,
    std::uint16_t event_timer_ms)
{
    /* Two event periods plus margin allow an arbitrary first-frame phase and
     * still require a second frame for interval verification. */
    const std::uint32_t timeout_ms =
        static_cast<std::uint32_t>(event_timer_ms) * 2U
        + kEventCollectionMarginMs;
    /* Release the mutex while waiting for the restored event-driven frames. */
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(
            lock, std::chrono::milliseconds(timeout_ms), [state]() {
                return state->failed || state->tpdo_count >= kEventSampleCount;
            })) {
        spdlog::error(
            "A04 restored event TPDO1 timed out: received={}/{}",
            state->tpdo_count, kEventSampleCount);
        return false;
    }
    if (state->failed) {
        lock.unlock();
        return validateCallbackState(state);
    }
    if (state->sync_count != 0U) {
        spdlog::error(
            "A04 unexpected SYNC while checking restored event TPDO1: {}",
            state->sync_count);
        return false;
    }

    /* Only the interval between the first two restored frames is checked; the
     * first frame may occur early because the event timer phase was reset. */
    const auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(
        state->tpdo_timestamps[1] - state->tpdo_timestamps[0]);
    /* Signed milliseconds simplify lower-bound arithmetic around tolerance. */
    const std::int64_t interval_ms = interval.count();
    /* Lower accepted bound around the saved event timer. */
    const std::int64_t minimum_ms =
        static_cast<std::int64_t>(event_timer_ms)
        - static_cast<std::int64_t>(kEventTimerToleranceMs);
    /* Upper accepted bound around the saved event timer. */
    const std::int64_t maximum_ms =
        static_cast<std::int64_t>(event_timer_ms)
        + static_cast<std::int64_t>(kEventTimerToleranceMs);
    if (interval_ms < minimum_ms || interval_ms > maximum_ms) {
        spdlog::error(
            "A04 restored event TPDO1 period out of tolerance: "
            "expected={}+/-{} ms actual={} ms",
            event_timer_ms, kEventTimerToleranceMs, interval_ms);
        return false;
    }

    spdlog::info("A04 restored event TPDO1 interval={} ms", interval_ms);
    return true;
}

} // namespace

int syncPdoProcess(lely::canopen::AsyncMaster& master)
{
    /* Overall process result starts at success and is latched to failure while
     * cleanup continues whenever enough state is known to restore safely. */
    int result = 0;
    /* false until all TPDO1 communication parameters have been uploaded. */
    bool snapshot_saved = false;
    /* false until the first TPDO1-modifying SDO is attempted. Once true, the
     * slave must be treated as potentially changed even if the local callback
     * never confirms whether that first write reached the remote OD. */
    bool tpdo_modification_attempted = false;
    /* true only after disable -> type 1 -> enable completes successfully; this
     * gates the restored event-driven regression check. */
    bool temporary_configuration_completed = false;
    /* false until strict read-back proves every saved TPDO1 field is restored. */
    bool restoration_verified = false;
    /* false means remote SDO completion state is known. A local wait timeout
     * sets this because issuing another SDO could overlap an unknown request. */
    bool completion_wait_timed_out = false;
    /* false until A04 installs its SYNC/TPDO callbacks; cleanup uses this flag
     * to avoid clearing callbacks that were never registered. */
    bool callbacks_registered = false;
    /* false until the master's original local 0x1006 period is read safely. */
    bool local_sync_period_saved = false;
    /* false until A04 writes local 0x1006; only then is restoration required. */
    bool local_sync_period_touched = false;
    /* false means the slave is expected Operational; true tracks states such as
     * Pre-operational or post-reset that require an explicit NMT Start. */
    bool slave_needs_start = false;
    /* Zero is a neutral placeholder until remote 0x1005 is uploaded. */
    std::uint32_t remote_sync_cob_id = 0;
    /* Zero is only initial storage; the exact pre-test local 0x1006 value is
     * saved before A04 changes SYNC generation. */
    std::uint32_t original_local_sync_period = 0;
    /* Value-initialize all snapshot fields so no partially read data is ever
     * mistaken for a valid saved configuration. */
    TpdoCommSnapshot original{};
    /* Observation state starts empty and is shared with event-loop callbacks
     * throughout quiet, synchronous, and restored-event phases. */
    const auto observation = std::make_shared<SyncPdoObservation>();

    /* Step 1 - Topology check: prove the local master is the only SYNC
     * producer, the slave remains a consumer, and both use the same COB-ID and
     * counter format. Save the local producer period for final restoration. */
    if (!validateSyncTopology(master, remote_sync_cob_id,
                              original_local_sync_period,
                              local_sync_period_saved,
                              completion_wait_timed_out)) {
        result = 1;
    }

    /* Step 2 - Keep the local master Operational. Moving the master itself to
     * Pre-operational could disable the PDO services needed to observe TPDO1. */
    if (result == 0
        && !issueNmtCommand(master, lely::canopen::NmtCommand::START,
                            CANOPEN_MASTER_NODE_ID,
                            "A04 local master NMT Start")) {
        result = 1;
    }

    /* Step 3 - Enter slave Pre-operational and wait for a fresh remote PREOP
     * state indication before changing TPDO1 communication parameters. Once
     * the transition is attempted, cleanup must assume the slave may need an
     * explicit Start even if state confirmation later times out. */
    if (result == 0) {
        slave_needs_start = true;
        if (!issueNmtCommandAndWaitForState(
                master, lely::canopen::NmtCommand::ENTER_PREOP,
                CANOPEN_SLAVE_NODE_ID, lely::canopen::NmtState::PREOP,
                std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS),
                "A04 slave NMT Enter Pre-operational")) {
            result = 1;
        }
    }

    /* Step 4 - Save the complete TPDO1 communication snapshot before the first
     * write so every temporary change can be verified and rolled back. */
    if (result == 0) {
        if (!readTpdoSnapshot(master, original,
                              completion_wait_timed_out)) {
            result = 1;
        } else {
            snapshot_saved = true;
            spdlog::info(
                "A04 saved TPDO1: cobid=0x{:08x} type={} inhibit={} "
                "event={} sync_start={}",
                original.cob_id,
                static_cast<unsigned int>(original.transmission_type),
                original.inhibit_time, original.event_timer,
                static_cast<unsigned int>(original.sync_start));
        }
    }

    /* Step 5 - Validate baseline assumptions required by this exact test:
     * TPDO1 is enabled, event-driven, and has a nonzero event timer that can be
     * used after restoration as an observable regression check. */
    if (result == 0 && (original.cob_id & kTpdoInvalidMask) != 0U) {
        spdlog::error("A04 TPDO1 is disabled in the original configuration");
        result = 1;
    }
    if (result == 0
        && original.transmission_type < kEventTransmissionTypeMinimum) {
        spdlog::error(
            "A04 expected an event-driven original TPDO1 type, actual={}",
            static_cast<unsigned int>(original.transmission_type));
        result = 1;
    }
    if (result == 0 && original.event_timer == 0U) {
        spdlog::error(
            "A04 original TPDO1 event timer is zero; restored periodic "
            "behavior cannot be verified");
        result = 1;
    }

    /* Step 6 - Force the local SYNC period to zero before enabling synchronous
     * TPDO1. This creates a deterministic no-SYNC observation window. */
    if (result == 0) {
        if (!writeLocalSyncPeriod(master, 0U)) {
            result = 1;
        } else {
            local_sync_period_touched = true;
        }
    }

    /* Step 7 - Temporarily configure slave TPDO1 using the legal
     * disable -> transmission type 1 -> enable sequence. */
    if (result == 0
        && !configureSynchronousTpdo(master, original.cob_id,
                                     tpdo_modification_attempted,
                                     completion_wait_timed_out)) {
        result = 1;
    } else if (result == 0) {
        temporary_configuration_completed = true;
    }

    /* Step 8 - Read back the two fields that A04 intentionally changed before
     * leaving Pre-operational. This catches rejected/partial configuration. */
    if (result == 0) {
        /* Zero is only a placeholder until 0x1800:01 read-back succeeds. */
        std::uint32_t configured_cob_id = 0;
        /* Zero is only a placeholder until 0x1800:02 read-back succeeds. */
        std::uint8_t configured_type = 0;
        /* Reuse one result variable because the two read-backs are sequential. */
        SdoOperationResult read_result =
            readRemoteSdo(master, CANOPEN_SLAVE_NODE_ID, kTpdoCommIndex,
                          kTpdoCobIdSubindex, configured_cob_id);
        if (read_result == SdoOperationResult::WAIT_TIMEOUT) {
            completion_wait_timed_out = true;
            result = 1;
        } else if (read_result != SdoOperationResult::SUCCESS) {
            result = 1;
        }

        if (result == 0) {
            read_result = readRemoteSdo(
                master, CANOPEN_SLAVE_NODE_ID, kTpdoCommIndex,
                kTpdoTransmissionTypeSubindex, configured_type);
            if (read_result == SdoOperationResult::WAIT_TIMEOUT) {
                completion_wait_timed_out = true;
                result = 1;
            } else if (read_result != SdoOperationResult::SUCCESS) {
                result = 1;
            }
        }

        if (result == 0
            && (configured_cob_id != original.cob_id
                || configured_type != kSynchronousTransmissionType)) {
            spdlog::error(
                "A04 synchronous TPDO1 read-back mismatch: cobid=0x{:08x} "
                "type={}",
                configured_cob_id,
                static_cast<unsigned int>(configured_type));
            result = 1;
        }
    }

    /* Step 9 - Register fresh observations and start the slave. Synchronous
     * TPDO behavior is meaningful only while the slave is Operational. */
    if (result == 0) {
        registerCallbacks(master, observation);
        callbacks_registered = true;
        resetObservation(observation);

        if (!issueNmtCommand(master, lely::canopen::NmtCommand::START,
                             CANOPEN_SLAVE_NODE_ID,
                             "A04 slave NMT Start")) {
            result = 1;
        } else {
            slave_needs_start = false;
        }
    }

    /* Step 10 - No-SYNC window: prove type-1 TPDO1 does not transmit from its
     * old event timer while the SYNC producer is disabled. */
    if (result == 0) {
        /* Use at least 1500 ms and always exceed the saved event timer by two
         * tolerance margins so stale event-driven behavior would be visible. */
        const std::uint32_t quiet_window_ms =
            std::max(kMinimumQuietWindowMs,
                     static_cast<std::uint32_t>(original.event_timer)
                         + 2U * kEventTimerToleranceMs);
        if (!verifyQuietWindow(observation, quiet_window_ms)) {
            result = 1;
        }
    }

    /* Step 11 - SYNC test: reset observations, start 200 ms local SYNC, collect
     * five SYNC/TPDO pairs, validate one TPDO inside each SYNC window, and then
     * confirm no sixth pair appears after the callback stops the producer. */
    if (result == 0) {
        resetObservation(observation);
        if (!writeLocalSyncPeriod(master, kSyncPeriodUs)) {
            result = 1;
        } else if (!waitForSynchronousSamples(observation)) {
            result = 1;
        } else if (!validateSynchronousTiming(observation)) {
            result = 1;
        } else if (!verifyPostSyncSilence(observation)) {
            result = 1;
        }
    }

    /* Step 12 - Stop local SYNC before any TPDO restoration work. Even on a
     * failed assertion, cleanup should not keep stimulating synchronous TPDOs. */
    if (local_sync_period_touched
        && !writeLocalSyncPeriod(master, 0U)) {
        result = 1;
    }

    /* Step 13 - Normal restoration path: when SDO completion state is known,
     * return the slave to confirmed Pre-operational, restore the original
     * transmission type/COB-ID, and strictly read back all saved TPDO/SYNC fields. Any
     * failure leaves restoration_verified false so Step 14 can establish a
     * stronger Reset Communication recovery boundary. */
    if (snapshot_saved && tpdo_modification_attempted
        && !completion_wait_timed_out) {
        /* true means normal restore can continue; any failed prerequisite
         * stops further normal-path SDO traffic and delegates to Step 14. */
        bool normal_restoration_ready = true;
        if (!slave_needs_start) {
            if (!issueNmtCommandAndWaitForState(
                    master, lely::canopen::NmtCommand::ENTER_PREOP,
                    CANOPEN_SLAVE_NODE_ID, lely::canopen::NmtState::PREOP,
                    std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS),
                    "A04 cleanup slave NMT Enter Pre-operational")) {
                result = 1;
                normal_restoration_ready = false;
            } else {
                slave_needs_start = true;
            }
        }

        if (normal_restoration_ready
            && !restoreTpdoConfiguration(master, original,
                                         completion_wait_timed_out)) {
            spdlog::error("A04 TPDO1 restoration write failed");
            result = 1;
            normal_restoration_ready = false;
        }

        if (normal_restoration_ready && !completion_wait_timed_out) {
            if (!verifyRestoredTpdoState(master, original,
                                         remote_sync_cob_id,
                                         completion_wait_timed_out)) {
                result = 1;
            } else {
                restoration_verified = true;
            }
        }
    }

    /* Step 14 - Recovery path: after any TPDO1 modification attempt, failure
     * to prove exact restoration is unsafe regardless of whether the cause was
     * WAIT_TIMEOUT, SDO abort/timeout, NMT transition failure, or read-back
     * mismatch. Reset Communication establishes a fresh protocol/configuration
     * boundary, then Boot and full read-back must prove the original state. */
    if (snapshot_saved && tpdo_modification_attempted
        && !restoration_verified) {
        result = 1;
        if (recoverTpdoStateWithReset(master, original, remote_sync_cob_id,
                                      completion_wait_timed_out,
                                      slave_needs_start)) {
            restoration_verified = true;
        }
    }

    /* A remaining unknown completion state is a cleanup failure even if earlier
     * protocol assertions happened to pass. */
    if (completion_wait_timed_out) {
        spdlog::error(
            "A04 local SDO completion state remains unknown; TPDO1 "
            "restoration cannot be verified");
        result = 1;
    }

    /* Step 15 - Event regression is meaningful only if type 1 was fully active,
     * restoration was verified, and callbacks are still available to observe
     * the restored event-driven TPDO frames. */
    const bool verify_restored_event =
        temporary_configuration_completed && restoration_verified
        && callbacks_registered;
    if (verify_restored_event) {
        resetObservation(observation);
    }

    /* Step 16 - Restore the slave to Operational only when either no TPDO1
     * modification was attempted or exact restoration has been verified. This
     * prevents an unverified synchronous/disabled TPDO configuration from being
     * reactivated after a failed cleanup. */
    const bool slave_start_is_safe =
        !tpdo_modification_attempted || restoration_verified;
    if (slave_needs_start && slave_start_is_safe) {
        if (!issueNmtCommand(master, lely::canopen::NmtCommand::START,
                             CANOPEN_SLAVE_NODE_ID,
                             "A04 cleanup slave NMT Start")) {
            result = 1;
        } else {
            slave_needs_start = false;
        }
    } else if (slave_needs_start && !slave_start_is_safe) {
        spdlog::error(
            "A04 refusing NMT Start because TPDO1 restoration is unverified");
        result = 1;
    }

    /* Step 17 - With SYNC still disabled, verify the restored event timer using
     * two TPDO1 frames and one measured inter-frame interval. */
    if (verify_restored_event && !slave_needs_start
        && !verifyRestoredEventTpdo(observation, original.event_timer)) {
        result = 1;
    }

    /* Never hide an unverified restoration behind another test result. */
    if (tpdo_modification_attempted && !restoration_verified) {
        spdlog::error(
            "A04 TPDO1 may remain changed because restoration was not "
            "verified");
        result = 1;
    }

    /* Step 18 - Remove A04 callbacks before releasing the shared observation
     * state or allowing later processes to reuse the same master hooks. */
    if (callbacks_registered) {
        clearCallbacks(master);
    }

    /* Step 19 - Restore the master's exact original 0x1006 period last. A
     * nonzero pre-test period would otherwise generate SYNC during the restored
     * event-driven TPDO regression window. */
    if (local_sync_period_touched && local_sync_period_saved
        && !writeLocalSyncPeriod(master, original_local_sync_period)) {
        result = 1;
    }

    if (result == 0) {
        spdlog::info("A04 SYNC consumer/synchronous TPDO1 test passed");
    }
    return result;
}
