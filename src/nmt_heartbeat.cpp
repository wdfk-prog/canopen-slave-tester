/**
 * @file
 * @brief Implements Boot, Heartbeat, and related EMCY test handling.
 */

#include "nmt_heartbeat.h"

#include "canopen_config.h"
#include "canopen_nmt.h"
#include "canopen_sdo.h"

#include <lely/coapp/master.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

namespace {

/** OD 0x1017 contains the Producer Heartbeat time. */
constexpr std::uint16_t kHeartbeatIndex = 0x1017;
/** Producer Heartbeat time is a scalar object at sub-index 0. */
constexpr std::uint8_t kHeartbeatSubindex = 0x00;
/** Three seconds allows the configured 500 ms heartbeat to miss multiple
 * periods before the test declares timeout. */
constexpr std::uint32_t kHeartbeatTimeoutMs = 3000;
/** Five normal master heartbeat periods are allowed to elapse before fault
 * injection, providing settling time for the slave consumer to enter ACTIVE. */
constexpr std::uint32_t kHeartbeatSampleCount = 5;
/** The test uses a 500 ms heartbeat period to keep timeout tests short while
 * leaving clear separation between consecutive heartbeat events. */
constexpr std::uint16_t kHeartbeatPeriodMs = 500;
/** Local object 0x1F81 controls the master's slave assignment policy. */
constexpr std::uint16_t kSlaveAssignmentIndex = 0x1F81;
/** 0x1F81 uses the node-ID as the sub-index for the managed slave. */
constexpr std::uint8_t kSlaveAssignmentSubindex = CANOPEN_SLAVE_NODE_ID;
/** 0x00000005 restores the exact slave-assignment value configured in the
 * generated master DCF for node 1; no bit semantics are inferred here. */
constexpr std::uint32_t kSlaveAssignmentEnabled = 0x00000005;
/** Zero temporarily removes the slave from Lely error-action handling so the
 * synthetic heartbeat timeout does not trigger an automatic reset. */
constexpr std::uint32_t kSlaveAssignmentDisabled = 0x00000000;
/** CANopen error code expected when a heartbeat consumer times out. */
constexpr std::uint16_t kHeartbeatConsumerEmcyCode = 0x8130;
/** EMCY code zero denotes the corresponding error-reset indication. */
constexpr std::uint16_t kEmcyResetCode = 0x0000;

/** Protects Boot result flags shared with the Lely event-loop callback. */
std::mutex g_boot_mutex;
/** Wakes control-thread waits when a Boot callback updates the result. */
std::condition_variable g_boot_condition;
/** false means no Boot callback has been observed since prepareBootWait(). */
bool g_boot_received = false;
/** false is the conservative default until an accepted Boot status arrives. */
bool g_boot_succeeded = false;

/** Protects expected/observed remote heartbeat supervision state. */
std::mutex g_heartbeat_mutex;
/** Wakes the process when the requested heartbeat timeout/recovery occurs. */
std::condition_variable g_heartbeat_condition;
/** false initially selects recovery; every test step overwrites it before
 * waiting for a concrete heartbeat event. */
bool g_expected_heartbeat_occurred = false;
/** false means the currently selected heartbeat event has not arrived yet. */
bool g_expected_heartbeat_received = false;

/** Protects expected/observed EMCY state shared with the event loop. */
std::mutex g_emcy_mutex;
/** Wakes the process when the selected EMCY code is received. */
std::condition_variable g_emcy_condition;
/** Start with the benign reset code; prepareEmcyWait() always selects the
 * actual expected code before each assertion. */
std::uint16_t g_expected_emcy_code = kEmcyResetCode;
/** false means the selected EMCY code has not been observed yet. */
bool g_expected_emcy_received = false;

/**
 * @brief Store Boot completion reported by the Lely event-loop thread.
 *
 * @param node_id Node that completed the Boot procedure.
 * @param state NMT state reported with the Boot result.
 * @param status Lely Boot status; zero and 'L' are accepted outcomes.
 * @param diagnostic Diagnostic text supplied by Lely.
 */
void bootCallback(std::uint8_t node_id, lely::canopen::NmtState state,
                  char status, const std::string& diagnostic) noexcept
{
    if (node_id != CANOPEN_SLAVE_NODE_ID) {
        return;
    }

    /* Lely reports status 0 for normal Boot completion and 'L' when the node
     * is already operational; both prove that startup management succeeded. */
    const bool succeeded = status == 0 || status == 'L';
    {
        /* Serialize callback publication with waitForBootCompletion(). */
        std::lock_guard<std::mutex> lock(g_boot_mutex);
        g_boot_received = true;
        g_boot_succeeded = succeeded;
    }
    g_boot_condition.notify_all();

    if (status == 0) {
        spdlog::info("Boot callback: node={} state=0x{:02x} status=success",
                     static_cast<unsigned int>(node_id),
                     static_cast<unsigned int>(state));
    } else if (status == 'L') {
        spdlog::info(
            "Boot callback: node={} state=0x{:02x} status=L "
            "(already operational)",
            static_cast<unsigned int>(node_id),
            static_cast<unsigned int>(state));
    } else {
        spdlog::error(
            "Boot callback: node={} state=0x{:02x} status={} "
            "diagnostic={}",
            static_cast<unsigned int>(node_id),
            static_cast<unsigned int>(state),
            static_cast<unsigned int>(static_cast<unsigned char>(status)),
            diagnostic);
    }
}

/**
 * @brief Log remote Heartbeat timeout and recovery events.
 *
 * @param node_id Node whose Heartbeat supervision changed.
 * @param occurred true for timeout; false for recovery.
 */
void heartbeatCallback(std::uint8_t node_id, bool occurred) noexcept
{
    if (node_id != CANOPEN_SLAVE_NODE_ID) {
        return;
    }

    /* false prevents unrelated heartbeat events from waking the current test
     * wait; it becomes true only for the explicitly selected transition. */
    bool matched = false;
    {
        /* Compare and publish the event while holding the shared-state lock. */
        std::lock_guard<std::mutex> lock(g_heartbeat_mutex);
        if (occurred == g_expected_heartbeat_occurred) {
            g_expected_heartbeat_received = true;
            matched = true;
        }
    }
    if (matched) {
        g_heartbeat_condition.notify_all();
    }

    if (occurred) {
        spdlog::error("Remote heartbeat timeout: node={}",
                      static_cast<unsigned int>(node_id));
    } else {
        spdlog::info("Remote heartbeat recovered: node={}",
                     static_cast<unsigned int>(node_id));
    }
}

/**
 * @brief Log node EMCY data and notify a matching Heartbeat test wait.
 *
 * @param node_id Node that produced the EMCY message.
 * @param error_code EMCY error code.
 * @param error_register Remote error register value.
 * @param manufacturer_data Five manufacturer-specific EMCY bytes.
 */
void emcyCallback(std::uint8_t node_id, std::uint16_t error_code,
                  std::uint8_t error_register,
                  std::uint8_t manufacturer_data[5]) noexcept
{
    if (node_id != CANOPEN_SLAVE_NODE_ID) {
        return;
    }

    /* Ignore EMCY traffic unrelated to the code selected by prepareEmcyWait(). */
    bool matched = false;
    {
        /* Protect the expected code and received flag from concurrent access. */
        std::lock_guard<std::mutex> lock(g_emcy_mutex);
        if (error_code == g_expected_emcy_code) {
            g_expected_emcy_received = true;
            matched = true;
        }
    }
    if (matched) {
        g_emcy_condition.notify_all();
    }

    spdlog::info(
        "EMCY callback: node={} code=0x{:04x} error_register=0x{:02x} "
        "manufacturer={:02x} {:02x} {:02x} {:02x} {:02x}",
        static_cast<unsigned int>(node_id),
        static_cast<unsigned int>(error_code),
        static_cast<unsigned int>(error_register),
        static_cast<unsigned int>(manufacturer_data[0]),
        static_cast<unsigned int>(manufacturer_data[1]),
        static_cast<unsigned int>(manufacturer_data[2]),
        static_cast<unsigned int>(manufacturer_data[3]),
        static_cast<unsigned int>(manufacturer_data[4]));
}

/**
 * @brief Select the next expected EMCY code and discard any older result.
 *
 * @param expected_error_code EMCY code expected by the automatic test.
 */
void prepareEmcyWait(std::uint16_t expected_error_code)
{
    /* Reset both selector and completion state atomically before an operation
     * that can immediately generate the expected EMCY. */
    std::lock_guard<std::mutex> lock(g_emcy_mutex);
    g_expected_emcy_code = expected_error_code;
    g_expected_emcy_received = false;
}

/**
 * @brief Wait for the selected EMCY code.
 *
 * @param timeout Maximum wait duration.
 * @return true when the selected code is received; otherwise false.
 */
bool waitForEmcy(std::chrono::milliseconds timeout)
{
    /* unique_lock is required because condition_variable::wait_for releases
     * the mutex while the event-loop thread executes the callback. */
    std::unique_lock<std::mutex> lock(g_emcy_mutex);
    return g_emcy_condition.wait_for(
        lock, timeout, []() { return g_expected_emcy_received; });
}

/**
 * @brief Select the next expected remote Heartbeat event.
 *
 * @param occurred true for timeout; false for recovery.
 */
void prepareHeartbeatWait(bool occurred)
{
    /* Set the expected direction and clear stale completion before changing
     * heartbeat producer state. */
    std::lock_guard<std::mutex> lock(g_heartbeat_mutex);
    g_expected_heartbeat_occurred = occurred;
    g_expected_heartbeat_received = false;
}

/**
 * @brief Wait for the selected remote Heartbeat event.
 *
 * @param timeout Maximum wait duration.
 * @return true when the selected event is received; otherwise false.
 */
bool waitForHeartbeat(std::chrono::milliseconds timeout)
{
    /* Allow the event-loop thread to acquire the mutex and publish the event
     * while this control thread waits. */
    std::unique_lock<std::mutex> lock(g_heartbeat_mutex);
    return g_heartbeat_condition.wait_for(
        lock, timeout, []() { return g_expected_heartbeat_received; });
}

/**
 * @brief Write the slave Producer Heartbeat time through the common SDO helper.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param period_ms Producer Heartbeat time written to 0x1017:00.
 * @return true when the SDO download completes successfully; otherwise false.
 */
bool writeSlaveProducerHeartbeat(lely::canopen::AsyncMaster& master,
                                 std::uint16_t period_ms)
{
    /* A01 keeps its existing 3 s end-to-end wait contract; a zero completion
     * margin preserves that behavior while reusing the common SDO adapter. */
    const SdoOperationResult result = writeRemoteSdo<std::uint16_t>(
        master, CANOPEN_SLAVE_NODE_ID, kHeartbeatIndex, kHeartbeatSubindex,
        period_ms, std::chrono::milliseconds(kHeartbeatTimeoutMs),
        std::chrono::milliseconds(0));
    if (result != SdoOperationResult::SUCCESS) {
        spdlog::error(
            "Unable to update slave producer heartbeat through SDO");
        return false;
    }

    return true;
}

/**
 * @brief Temporarily update the local NMT slave assignment used on errors.
 *
 * Lely resets an optional slave when its Heartbeat times out. Clearing the
 * network-list assignment during the synthetic timeout keeps the SDO service
 * available so recovery is driven by the explicit 0x1017:00 SDO write.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param assignment Value written to local 0x1F81 for node 1.
 * @return true on success; otherwise false.
 */
bool writeSlaveAssignment(lely::canopen::AsyncMaster& master,
                          std::uint32_t assignment)
{
    /* Local OD Write is synchronous and does not send an SDO to the slave;
     * the default error_code represents success until Lely reports otherwise. */
    std::error_code error;
    master.Write<std::uint32_t>(kSlaveAssignmentIndex,
                                kSlaveAssignmentSubindex, assignment, error);
    if (error) {
        spdlog::error("Unable to update local slave assignment: {}",
                      error.message());
        return false;
    }
    return true;
}

/**
 * @brief Verify master-side supervision of the slave Producer Heartbeat.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @return Zero on success; otherwise a non-zero process result.
 */
int testSlaveProducerHeartbeat(lely::canopen::AsyncMaster& master)
{
    /* Start optimistic and retain any failed assertion through cleanup. */
    int result = 0;

    /* Step 1: disable Lely's automatic error action for the slave so the
     * deliberate heartbeat loss can be observed without an implicit reset. */
    if (!writeSlaveAssignment(master, kSlaveAssignmentDisabled)) {
        return 1;
    }

    /* Step 2: stop the slave Producer Heartbeat remotely and wait for the
     * master's heartbeat consumer timeout callback. */
    prepareHeartbeatWait(true);
    if (!writeSlaveProducerHeartbeat(master, 0)) {
        (void)writeSlaveAssignment(master, kSlaveAssignmentEnabled);
        return 1;
    }
    spdlog::info("Slave producer heartbeat stopped through SDO");

    if (!waitForHeartbeat(
            std::chrono::milliseconds(kHeartbeatTimeoutMs))) {
        spdlog::error("Remote heartbeat timeout event timed out");
        result = 1;
    } else {
        spdlog::info("Master detected slave heartbeat timeout");
    }

    /* Step 3: restore the slave Producer Heartbeat and require the matching
     * recovery callback from the master's consumer. */
    prepareHeartbeatWait(false);
    if (!writeSlaveProducerHeartbeat(
            master,
            kHeartbeatPeriodMs)) {
        spdlog::error(
            "Slave producer heartbeat may remain disabled until reset");
        result = 1;
    } else {
        spdlog::info("Slave producer heartbeat restored through SDO");
        if (!waitForHeartbeat(
                std::chrono::milliseconds(kHeartbeatTimeoutMs))) {
            spdlog::error("Remote heartbeat recovery event timed out");
            result = 1;
        } else {
            spdlog::info("Master detected slave heartbeat recovery");
        }
    }

    /* Step 4: always restore the master's original slave assignment policy. */
    if (!writeSlaveAssignment(master, kSlaveAssignmentEnabled)) {
        result = 1;
    }

    return result;
}

} // namespace

void registerNmtHeartbeatCallbacks(lely::canopen::AsyncMaster& master)
{
    master.OnBoot(bootCallback);
    master.OnHeartbeat(heartbeatCallback);
    master.OnEmcy(emcyCallback);
}

void prepareBootWait()
{
    /* Clear both flags together so no result from an earlier Boot procedure
     * can satisfy a later reset/startup wait. */
    std::lock_guard<std::mutex> lock(g_boot_mutex);
    g_boot_received = false;
    g_boot_succeeded = false;
}

bool waitForBootCompletion(std::chrono::milliseconds timeout)
{
    /* Release the mutex while waiting so bootCallback() can publish the new
     * Boot result on the event-loop thread. */
    std::unique_lock<std::mutex> lock(g_boot_mutex);
    if (!g_boot_condition.wait_for(
            lock, timeout, []() { return g_boot_received; })) {
        return false;
    }
    return g_boot_succeeded;
}

int heartbeatProcess(lely::canopen::AsyncMaster& master)
{
    /* Preserve the first failure while still running recovery steps. */
    int result = 0;
    /* Local OD writes report errors synchronously through this reusable code;
     * default construction starts in the success state. */
    std::error_code error;

    /* Step 1: allow the slave Heartbeat consumer to receive several normal
     * master heartbeats and enter ACTIVE before fault injection. Five 500 ms
     * periods are long enough to establish normal supervision first. */
    std::this_thread::sleep_for(std::chrono::milliseconds(
        kHeartbeatPeriodMs * kHeartbeatSampleCount));

    /* Step 2: stop the master's local Producer Heartbeat (0x1017 = 0) and
     * verify the slave reports heartbeat-consumer timeout EMCY 0x8130. */
    prepareEmcyWait(kHeartbeatConsumerEmcyCode);
    master.Write<std::uint16_t>(kHeartbeatIndex,
                                kHeartbeatSubindex,
                                static_cast<std::uint16_t>(0), error);
    if (error) {
        spdlog::error("Unable to stop master producer heartbeat: {}",
                      error.message());
        return 1;
    }
    spdlog::info("Master producer heartbeat stopped");

    if (!waitForEmcy(std::chrono::milliseconds(kHeartbeatTimeoutMs))) {
        spdlog::error("Heartbeat consumer EMCY 0x8130 timed out");
        result = 1;
    } else {
        spdlog::info("Remote node detected master heartbeat timeout");
    }

    /* Step 3: restore the master's 500 ms heartbeat and require the slave's
     * EMCY reset indication before moving on. */
    prepareEmcyWait(kEmcyResetCode);
    error.clear();
    master.Write<std::uint16_t>(
        kHeartbeatIndex, kHeartbeatSubindex,
        kHeartbeatPeriodMs, error);
    if (error) {
        spdlog::error("Unable to restore master producer heartbeat: {}",
                      error.message());
        return 1;
    }
    spdlog::info("Master producer heartbeat restored");

    if (!waitForEmcy(std::chrono::milliseconds(kHeartbeatTimeoutMs))) {
        spdlog::error("Heartbeat consumer EMCY reset timed out");
        return 1;
    }
    spdlog::info("Remote heartbeat error cleared");

    /* Step 4: put the slave in Operational before testing the opposite
     * direction, where the master supervises the slave heartbeat. */
    if (!issueNmtCommand(master, lely::canopen::NmtCommand::START,
                         CANOPEN_SLAVE_NODE_ID, "A01 slave NMT Start")) {
        return 1;
    }
    spdlog::info("NMT Start sent to node {}", CANOPEN_SLAVE_NODE_ID);

    /* Step 5: stop/restore the slave Producer Heartbeat through SDO and verify
     * the master's timeout and recovery callbacks. */
    if (testSlaveProducerHeartbeat(master) != 0) {
        result = 1;
    }

    return result;
}
