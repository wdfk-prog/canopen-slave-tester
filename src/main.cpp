/**
 * @file
 * @brief Implements the selectable CANopen master-tester and slave-peer entry.
 */

#include "canopen_config.h"
#include "canopen_emcy.h"
#include "canopen_nmt.h"
#include "canopen_process.h"
#include "emcy_process.h"
#include "nmt_heartbeat.h"
#include "nmt_master_process.h"
#include "pdo_process.h"
#include "sdo_process.h"
#include "shutdown_process.h"
#include "sync_pdo_process.h"
#include "time_process.h"

#include <lely/coapp/master.hpp>
#include <lely/coapp/slave.hpp>
#include <lely/ev/exec.hpp>
#include <lely/ev/loop.hpp>
#include <lely/io2/ctx.hpp>
#include <lely/io2/linux/can.hpp>
#include <lely/io2/posix/poll.hpp>
#include <lely/io2/sys/io.hpp>
#include <lely/io2/sys/timer.hpp>

#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

namespace {

/** Ordered master-side automatic test process table. */
const std::array<CanopenProcessEntry,
                 CANOPEN_ENABLE_HEARTBEAT_PROCESS
                     + CANOPEN_ENABLE_SDO_PROCESS
                     + CANOPEN_ENABLE_PDO_PROCESS
                     + CANOPEN_ENABLE_SYNC_PDO_PROCESS
                     + CANOPEN_ENABLE_TIME_PROCESS
                     + CANOPEN_ENABLE_EMCY_PROCESS>
    g_canopen_processes = {{
#if CANOPEN_ENABLE_HEARTBEAT_PROCESS
        {"A01 Heartbeat", heartbeatProcess},
#endif /* CANOPEN_ENABLE_HEARTBEAT_PROCESS */
#if CANOPEN_ENABLE_SDO_PROCESS
        {"A02 SDO", sdoProcess},
#endif /* CANOPEN_ENABLE_SDO_PROCESS */
#if CANOPEN_ENABLE_PDO_PROCESS
        {"A03 PDO", pdoProcess},
#endif /* CANOPEN_ENABLE_PDO_PROCESS */
#if CANOPEN_ENABLE_SYNC_PDO_PROCESS
        {"A04 SYNC PDO", syncPdoProcess},
#endif /* CANOPEN_ENABLE_SYNC_PDO_PROCESS */
#if CANOPEN_ENABLE_TIME_PROCESS
        {"A05 TIME", timeProcess},
#endif /* CANOPEN_ENABLE_TIME_PROCESS */
#if CANOPEN_ENABLE_EMCY_PROCESS
        {"A06 EMCY", emcyProcess},
#endif /* CANOPEN_ENABLE_EMCY_PROCESS */
    }};

/** Signal-safe shutdown request flag; zero means no termination signal yet. */
volatile std::sig_atomic_t g_stop_requested = 0;
/** Event-loop liveness flag read by the role control thread. */
std::atomic<bool> g_canopen_loop_running(false);
/** Protects the event-loop startup publication. */
std::mutex g_canopen_loop_mutex;
/** Wakes the control thread when the worker has entered its run path. */
std::condition_variable g_canopen_loop_condition;
/** true after the current event-loop worker publishes startup. */
bool g_canopen_loop_started = false;

/**
 * @brief Request orderly shutdown from a POSIX signal handler.
 */
void signalHandler(int) noexcept
{
    g_stop_requested = 1;
}

/**
 * @brief Initialize the non-blocking asynchronous application logger.
 */
void initializeLogging()
{
    /* The queue and worker count come from project configuration so logging
     * remains non-blocking under CAN callback bursts. */
    spdlog::init_thread_pool(CANOPEN_LOG_QUEUE_SIZE,
                             CANOPEN_LOG_WORKER_COUNT);
    /* The single console sink keeps all process diagnostics on stdout. */
    const auto sink =
        std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    /* overrun_oldest bounds memory use if producers temporarily outpace the
     * asynchronous logger worker. */
    const auto logger = std::make_shared<spdlog::async_logger>(
        "canopen_master", sink, spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
}

/**
 * @brief Install the signal-safe shutdown handlers shared by both roles.
 *
 * @return true when both handlers are installed; otherwise false.
 */
bool installSignalHandlers()
{
    if (std::signal(SIGINT, signalHandler) == SIG_ERR
        || std::signal(SIGTERM, signalHandler) == SIG_ERR) {
        spdlog::error("Unable to install process signal handlers");
        return false;
    }
    return true;
}

/**
 * @brief Log CAN controller state transitions.
 */
void canStateCallback(lely::io::CanState new_state,
                      lely::io::CanState old_state) noexcept
{
    spdlog::warn("CAN state changed: old={} new={}",
                 static_cast<int>(old_state),
                 static_cast<int>(new_state));
}

/**
 * @brief Log CAN controller error flags.
 */
void canErrorCallback(lely::io::CanError error) noexcept
{
    spdlog::error("CAN error flags: 0x{:x}", static_cast<int>(error));
}

/**
 * @brief Run the Lely event loop until Context shutdown or an I/O error.
 */
void canopenWorker(lely::ev::Loop& loop) noexcept
{
    {
        /* Publish startup while holding the mutex used by the blocking waiter;
         * this avoids the polling startup race from the previous peer draft. */
        std::lock_guard<std::mutex> lock(g_canopen_loop_mutex);
        g_canopen_loop_started = true;
        g_canopen_loop_running.store(true, std::memory_order_release);
    }
    g_canopen_loop_condition.notify_all();

    /* A default-constructed error_code represents success until loop.run()
     * reports an I/O or executor failure. */
    std::error_code error;
    loop.run(error);
    if (error) {
        spdlog::error("CANopen event loop stopped with error: {}",
                      error.message());
    }

    g_canopen_loop_running.store(false, std::memory_order_release);
    g_canopen_loop_condition.notify_all();
}

/**
 * @brief Start the CANopen worker and wait for its startup publication.
 *
 * @param loop Lely event loop associated with the active role objects.
 * @param worker Receives the created worker thread.
 * @return true when the worker starts within the configured timeout.
 */
bool startCanopenWorker(lely::ev::Loop& loop, std::thread& worker)
{
    {
        std::lock_guard<std::mutex> lock(g_canopen_loop_mutex);
        g_canopen_loop_started = false;
        g_canopen_loop_running.store(false, std::memory_order_release);
    }

    worker = std::thread(canopenWorker, std::ref(loop));

    const std::chrono::milliseconds timeout(CANOPEN_LOOP_START_TIMEOUT_MS);
    std::unique_lock<std::mutex> lock(g_canopen_loop_mutex);
    return g_canopen_loop_condition.wait_for(
        lock, timeout, []() { return g_canopen_loop_started; });
}

/**
 * @brief Stop pending Lely I/O and join the event-loop worker.
 *
 * Context shutdown is retained intentionally: CanChannel and Timer register
 * asynchronous I/O services with the Context. Cancelling those services before
 * join lets loop.run() drain/terminate before the role-owned Lely objects and
 * their I/O resources leave scope.
 *
 * @param context Lely I/O context owning pending services.
 * @param worker Event-loop worker started by startCanopenWorker().
 */
void stopCanopenWorker(lely::io::Context& context, std::thread& worker) noexcept
{
    context.shutdown();
    if (worker.joinable()) {
        worker.join();
    }
}

/**
 * @brief Verify the configured SocketCAN nominal bitrate.
 *
 * @param controller Open Lely SocketCAN controller.
 * @return true when the current nominal bitrate matches the project setting.
 */
bool validateCanBitrate(lely::io::CanController& controller)
{
    /* error is reused for the synchronous bitrate query and starts clear. */
    std::error_code error;
    /* Zero initializes output storage before get_bitrate() overwrites it. */
    int nominal_bitrate = 0;
    /* CAN FD data bitrate is not validated here but must still be supplied as
     * an output argument to the controller API. */
    int data_bitrate = 0;
    controller.get_bitrate(&nominal_bitrate, &data_bitrate, error);
    if (error) {
        spdlog::error("Unable to read CAN bitrate: {}", error.message());
        return false;
    }
    if (nominal_bitrate != CANOPEN_EXPECTED_BITRATE) {
        spdlog::error("Unexpected CAN bitrate: expected={} actual={}",
                      CANOPEN_EXPECTED_BITRATE, nominal_bitrate);
        return false;
    }
    return true;
}

/**
 * @brief Run the existing AsyncMaster-based A01-A06 tester role.
 *
 * @param context Common Lely I/O context used to stop the worker on exit.
 * @param loop Common Lely event loop.
 * @param executor Executor associated with loop.
 * @param timer CANopen protocol timer dedicated to this role.
 * @param channel CAN channel dedicated to this role.
 * @return Zero on success; otherwise a non-zero application result.
 */
int runCanopenMaster(lely::io::Context& context, lely::ev::Loop& loop,
                     lely::ev::Executor executor, lely::io::Timer& timer,
                     lely::io::CanChannel& channel)
{
    /* Accumulate setup/process/shutdown failures; zero means the whole run is
     * still considered successful. */
    int error_count = 0;
    /* Remain false until the slave produces an accepted Boot callback after
     * master.Reset(); this gates every automatic process. */
    bool startup_boot_succeeded = false;
    /* The master owns local OD state, SDO/PDO services, NMT control, and all
     * callbacks used by the enabled A01-A06 processes. */
    lely::canopen::AsyncMaster master(
        executor, timer, channel, CANOPEN_MASTER_DCF_PATH, "",
        CANOPEN_MASTER_NODE_ID);
    master.OnCanState(canStateCallback);
    master.OnCanError(canErrorCallback);
    registerNmtStateCallback(master);
    registerNmtHeartbeatCallbacks(master);
    registerCanopenEmcyCallback(master);

    /* Run the event loop on a dedicated thread so process functions can wait
     * synchronously for callbacks without blocking Lely itself. */
    std::thread canopen_thread;
    if (!startCanopenWorker(loop, canopen_thread)) {
        spdlog::error("CANopen event loop did not start in time");
        stopCanopenWorker(context, canopen_thread);
        return 1;
    }

    /* Establish a clean Boot wait state before Reset can generate callbacks. */
    prepareBootWait();
    master.Reset();
    startup_boot_succeeded = waitForBootCompletion(
        std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS));
    if (!startup_boot_succeeded) {
        spdlog::error("Remote node did not complete Boot during startup");
        ++error_count;
    } else {
        spdlog::info("Remote node {} completed startup Boot",
                     CANOPEN_SLAVE_NODE_ID);
    }

    /* Execute only A01-A06 master-side processes. NMT-master validation is a
     * slave-role responsibility and is intentionally absent from this table. */
    if (startup_boot_succeeded) {
        error_count += canopenRunProcesses(master, g_canopen_processes.data(),
                                           g_canopen_processes.size());
    }

    /* Preserve the existing interactive lifetime: automatic validation ends
     * first, then the process remains available until Ctrl+C or loop failure. */
    spdlog::info("Automatic CANopen tests finished; waiting for Ctrl+C");
    while (g_stop_requested == 0
           && g_canopen_loop_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (g_stop_requested == 0
        && !g_canopen_loop_running.load(std::memory_order_acquire)) {
        spdlog::error(
            "CANopen event loop stopped before shutdown was requested");
        ++error_count;
    }

#if CANOPEN_ENABLE_FINAL_RESET_PROCESS
    /* Reset the slave only while the event loop can still deliver the Boot
     * callback needed to verify the final reset. */
    if (g_canopen_loop_running.load(std::memory_order_acquire)
        && startup_boot_succeeded) {
        error_count += finalResetProcess(master);
    }
#endif /* CANOPEN_ENABLE_FINAL_RESET_PROCESS */

    stopCanopenWorker(context, canopen_thread);
    spdlog::info("CANopen master exiting with result={}", error_count);
    return error_count == 0 ? 0 : 1;
}

/**
 * @brief Run the BasicSlave peer used to validate the MCU NMT-master behavior.
 *
 * @param context Common Lely I/O context used to stop the worker on exit.
 * @param loop Common Lely event loop.
 * @param executor Executor associated with loop.
 * @param timer CANopen protocol timer dedicated to this role.
 * @param channel CAN channel dedicated to this role.
 * @return Zero on success; otherwise a non-zero application result.
 */
int runCanopenSlave(lely::io::Context& context, lely::ev::Loop& loop,
                    lely::ev::Executor executor, lely::io::Timer& timer,
                    lely::io::CanChannel& channel)
{
    int error_count = 0;
    /* Reuse the MCU-provided project.eds unchanged and do not load a concise
     * DCF for the software peer. The validation process normalizes Node 2 with
     * MCU-issued NMT commands instead of changing EDS startup behavior. */
    lely::canopen::BasicSlave slave(
        executor, timer, channel, CANOPEN_PEER_EDS_PATH,
        "", CANOPEN_PEER_NODE_ID);
    slave.OnCanState(canStateCallback);
    slave.OnCanError(canErrorCallback);

    std::thread canopen_thread;
    if (!startCanopenWorker(loop, canopen_thread)) {
        spdlog::error("CANopen event loop did not start in time");
        stopCanopenWorker(context, canopen_thread);
        return 1;
    }

    /* BasicSlave::Reset() starts the normal slave boot-up sequence. Lely
     * exposes no status-returning overload, so this existing throwing API is
     * called directly rather than adding a local try/catch wrapper. */
    slave.Reset();

    spdlog::info("CANopen slave peer started: node={}",
                 CANOPEN_PEER_NODE_ID);

#if CANOPEN_ENABLE_NMT_MASTER_PROCESS
    /* This role is the controlled node. The DUT must initiate the NMT command
     * sequence; the peer only observes commands and verifies its own states. */
    error_count += nmtMasterProcess(slave);
#else
    spdlog::info("NMT master validation disabled; waiting for Ctrl+C");
    while (g_stop_requested == 0
           && g_canopen_loop_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif /* CANOPEN_ENABLE_NMT_MASTER_PROCESS */

    if (g_stop_requested == 0
        && !g_canopen_loop_running.load(std::memory_order_acquire)) {
        spdlog::error(
            "CANopen event loop stopped before the slave role completed");
        ++error_count;
    }

    stopCanopenWorker(context, canopen_thread);
    spdlog::info("CANopen slave peer exiting with result={}", error_count);
    return error_count == 0 ? 0 : 1;
}

} // namespace

int main(void)
{
    initializeLogging();
    if (!installSignalHandlers()) {
        spdlog::shutdown();
        return 1;
    }

    /* Both roles share one Lely/SocketCAN runtime setup. Only the CANopen node
     * object and test flow differ after this common initialization. */
    lely::io::IoGuard io_guard;
    lely::io::Context context;
    lely::io::Poll poll(context);
    lely::ev::Loop loop(poll.get_poll());
    lely::ev::Executor executor = loop.get_executor();
    lely::io::Timer timer(poll, executor, CLOCK_MONOTONIC);
    lely::io::CanController controller(CANOPEN_INTERFACE_NAME);
    if (!validateCanBitrate(controller)) {
        spdlog::shutdown();
        return 1;
    }

    lely::io::CanChannel channel(poll, executor,
                                 CANOPEN_CHANNEL_RX_QUEUE_SIZE);
    std::error_code error;
    channel.open(controller, lely::io::CanBusFlag::NONE, error);
    if (error) {
        spdlog::error("Unable to open CAN interface {}: {}",
                      CANOPEN_INTERFACE_NAME, error.message());
        spdlog::shutdown();
        return 1;
    }

    int result = 1;
    if (CANOPEN_ROLE == CANOPEN_ROLE_MASTER) {
        result = runCanopenMaster(context, loop, executor, timer, channel);
    } else {
        result = runCanopenSlave(context, loop, executor, timer, channel);
    }

    spdlog::shutdown();
    return result;
}
