/**
 * @file
 * @brief Implements Boot, Heartbeat, and related EMCY test handling.
 */

#include "nmt_heartbeat.h"

#include "canopen_config.h"

#include <lely/coapp/master.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

namespace {

constexpr std::uint16_t kProducerHeartbeatIndex = 0x1017;
constexpr std::uint8_t kProducerHeartbeatSubindex = 0x00;
constexpr std::uint16_t kSlaveAssignmentIndex = 0x1F81;
constexpr std::uint8_t kSlaveAssignmentSubindex = CANOPEN_SLAVE_NODE_ID;
constexpr std::uint32_t kSlaveAssignmentEnabled = 0x00000005;
constexpr std::uint32_t kSlaveAssignmentDisabled = 0x00000000;
constexpr std::uint16_t kHeartbeatConsumerEmcyCode = 0x8130;
constexpr std::uint16_t kEmcyResetCode = 0x0000;

std::mutex g_boot_mutex;
std::condition_variable g_boot_condition;
bool g_boot_received = false;
bool g_boot_succeeded = false;

std::mutex g_heartbeat_mutex;
std::condition_variable g_heartbeat_condition;
bool g_expected_heartbeat_occurred = false;
bool g_expected_heartbeat_received = false;

std::mutex g_emcy_mutex;
std::condition_variable g_emcy_condition;
std::uint16_t g_expected_emcy_code = kEmcyResetCode;
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

    const bool succeeded = status == 0 || status == 'L';
    {
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

    bool matched = false;
    {
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

    bool matched = false;
    {
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
    std::unique_lock<std::mutex> lock(g_heartbeat_mutex);
    return g_heartbeat_condition.wait_for(
        lock, timeout, []() { return g_expected_heartbeat_received; });
}

/**
 * @brief Write the slave Producer Heartbeat time through SDO and wait for it.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param period_ms Producer Heartbeat time written to 0x1017:00.
 * @return true when the SDO download completes successfully; otherwise false.
 */
bool writeSlaveProducerHeartbeat(lely::canopen::AsyncMaster& master,
                                 std::uint16_t period_ms)
{
    struct WriteState {
        std::mutex mutex;
        std::condition_variable condition;
        bool completed = false;
        std::error_code error;
    };

    const auto state = std::make_shared<WriteState>();
    std::error_code submit_error;
    master.SubmitWrite(
        master.GetExecutor(), CANOPEN_SLAVE_NODE_ID, kProducerHeartbeatIndex,
        kProducerHeartbeatSubindex, period_ms,
        [state](std::uint8_t, std::uint16_t, std::uint8_t,
                std::error_code error) noexcept {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->completed = true;
                state->error = error;
            }
            state->condition.notify_all();
        },
        std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS), submit_error);
    if (submit_error) {
        spdlog::error(
            "Unable to submit slave producer heartbeat SDO write: {}",
            submit_error.message());
        return false;
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(
            lock, std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS),
            [state]() { return state->completed; })) {
        spdlog::error("Slave producer heartbeat SDO write timed out");
        return false;
    }
    if (state->error) {
        spdlog::error("Slave producer heartbeat SDO write failed: {}",
                      state->error.message());
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
    int result = 0;

    if (!writeSlaveAssignment(master, kSlaveAssignmentDisabled)) {
        return 1;
    }

    prepareHeartbeatWait(true);
    if (!writeSlaveProducerHeartbeat(master, 0)) {
        (void)writeSlaveAssignment(master, kSlaveAssignmentEnabled);
        return 1;
    }
    spdlog::info("Slave producer heartbeat stopped through SDO");

    if (!waitForHeartbeat(
            std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS))) {
        spdlog::error("Remote heartbeat timeout event timed out");
        result = 1;
    } else {
        spdlog::info("Master detected slave heartbeat timeout");
    }

    prepareHeartbeatWait(false);
    if (!writeSlaveProducerHeartbeat(
            master,
            static_cast<std::uint16_t>(CANOPEN_HEARTBEAT_PERIOD_MS))) {
        spdlog::error(
            "Slave producer heartbeat may remain disabled until reset");
        result = 1;
    } else {
        spdlog::info("Slave producer heartbeat restored through SDO");
        if (!waitForHeartbeat(
                std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS))) {
            spdlog::error("Remote heartbeat recovery event timed out");
            result = 1;
        } else {
            spdlog::info("Master detected slave heartbeat recovery");
        }
    }

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
    std::lock_guard<std::mutex> lock(g_boot_mutex);
    g_boot_received = false;
    g_boot_succeeded = false;
}

bool waitForBootCompletion(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(g_boot_mutex);
    if (!g_boot_condition.wait_for(
            lock, timeout, []() { return g_boot_received; })) {
        return false;
    }
    return g_boot_succeeded;
}

int heartbeatProcess(lely::canopen::AsyncMaster& master)
{
    int result = 0;
    std::error_code error;

    // Allow the slave Heartbeat consumer to receive a normal master
    // Heartbeat and enter ACTIVE before testing timeout detection.
    std::this_thread::sleep_for(std::chrono::milliseconds(
        CANOPEN_HEARTBEAT_PERIOD_MS * 2));

    prepareEmcyWait(kHeartbeatConsumerEmcyCode);
    master.Write<std::uint16_t>(kProducerHeartbeatIndex,
                                kProducerHeartbeatSubindex,
                                static_cast<std::uint16_t>(0), error);
    if (error) {
        spdlog::error("Unable to stop master producer heartbeat: {}",
                      error.message());
        return 1;
    }
    spdlog::info("Master producer heartbeat stopped");

    if (!waitForEmcy(std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS))) {
        spdlog::error("Heartbeat consumer EMCY 0x8130 timed out");
        result = 1;
    } else {
        spdlog::info("Remote node detected master heartbeat timeout");
    }

    prepareEmcyWait(kEmcyResetCode);
    error.clear();
    master.Write<std::uint16_t>(
        kProducerHeartbeatIndex, kProducerHeartbeatSubindex,
        static_cast<std::uint16_t>(CANOPEN_HEARTBEAT_PERIOD_MS), error);
    if (error) {
        spdlog::error("Unable to restore master producer heartbeat: {}",
                      error.message());
        return 1;
    }
    spdlog::info("Master producer heartbeat restored");

    if (!waitForEmcy(std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS))) {
        spdlog::error("Heartbeat consumer EMCY reset timed out");
        return 1;
    }
    spdlog::info("Remote heartbeat error cleared");

    master.Command(lely::canopen::NmtCommand::START,
                   CANOPEN_SLAVE_NODE_ID);
    spdlog::info("NMT Start sent to node {}", CANOPEN_SLAVE_NODE_ID);

    if (testSlaveProducerHeartbeat(master) != 0) {
        result = 1;
    }

    return result;
}
