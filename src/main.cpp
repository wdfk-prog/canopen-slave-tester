/**
 * @file
 * @brief Implements CANopen master initialization and process orchestration.
 */

#include "canopen_config.h"
#include "canopen_nmt.h"
#include "canopen_process.h"
#include "nmt_heartbeat.h"
#include "pdo_process.h"
#include "sdo_process.h"
#include "sync_pdo_process.h"
#include "time_process.h"
#include "shutdown_process.h"

#include <lely/coapp/master.hpp>
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
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <system_error>
#include <thread>

namespace {
/** Ordered automatic test process table assembled from build-time switches. */
const std::array<CanopenProcessEntry,
                 CANOPEN_ENABLE_HEARTBEAT_PROCESS
                     + CANOPEN_ENABLE_SDO_PROCESS
                     + CANOPEN_ENABLE_PDO_PROCESS
                     + CANOPEN_ENABLE_SYNC_PDO_PROCESS
                     + CANOPEN_ENABLE_TIME_PROCESS>
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
    }};

/** Signal-safe shutdown request flag; zero means no termination signal yet. */
volatile std::sig_atomic_t g_stop_requested = 0;
/** Event-loop liveness flag shared between the worker and control thread. */
std::atomic<bool> g_canopen_loop_running(false);

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
    /* Publish liveness before entering the blocking event-loop run call. */
    g_canopen_loop_running.store(true, std::memory_order_release);

    /* A default-constructed error_code represents success until loop.run()
     * reports an I/O or executor failure. */
    std::error_code error;
    loop.run(error);
    if (error) {
        spdlog::error("CANopen event loop stopped with error: {}",
                      error.message());
    }

    g_canopen_loop_running.store(false, std::memory_order_release);
}

} // namespace

int main(void)
{
    /* Accumulate setup/process/shutdown failures; zero means the whole run is
     * still considered successful. */
    int error_count = 0;
    initializeLogging();

    /* Install only signal-safe handlers; the actual shutdown stays in the
     * normal control flow below. */
    if (std::signal(SIGINT, signalHandler) == SIG_ERR
        || std::signal(SIGTERM, signalHandler) == SIG_ERR) {
        spdlog::error("Unable to install process signal handlers");
        return 1;
    }

    /* io_guard initializes the Lely I/O subsystem for this process lifetime. */
    lely::io::IoGuard io_guard;
    /* context owns cancellable asynchronous I/O resources. */
    lely::io::Context context;
    /* poll binds the context to the POSIX polling backend used on Linux. */
    lely::io::Poll poll(context);
    /* loop dispatches all Lely CANopen callbacks on one event-loop thread. */
    lely::ev::Loop loop(poll.get_poll());
    /* executor is retained because every Lely async object must share the
     * event-loop execution context. */
    lely::ev::Executor executor = loop.get_executor();
    /* CLOCK_MONOTONIC prevents wall-clock adjustments from changing protocol
     * timer behavior. */
    lely::io::Timer timer(poll, executor, CLOCK_MONOTONIC);
    /* controller opens the configured SocketCAN interface by name. */
    lely::io::CanController controller(CANOPEN_INTERFACE_NAME);

    /* error is reused for synchronous Lely setup calls and starts clear. */
    std::error_code error;
    /* Zero initializes output storage before get_bitrate() overwrites it. */
    int nominal_bitrate = 0;
    /* CAN FD data bitrate is not validated here but must still be supplied as
     * an output argument to the controller API. */
    int data_bitrate = 0;
    controller.get_bitrate(&nominal_bitrate, &data_bitrate, error);
    if (error) {
        spdlog::error("Unable to read CAN bitrate: {}", error.message());
        return 1;
    }
    if (nominal_bitrate != CANOPEN_EXPECTED_BITRATE) {
        spdlog::error("Unexpected CAN bitrate: expected={} actual={}",
                      CANOPEN_EXPECTED_BITRATE, nominal_bitrate);
        return 1;
    }

    /* The queue depth is configured to absorb short receive bursts without
     * changing protocol ordering. */
    lely::io::CanChannel channel(poll, executor,
                                 CANOPEN_CHANNEL_RX_QUEUE_SIZE);
    channel.open(controller, lely::io::CanBusFlag::NONE, error);
    if (error) {
        spdlog::error("Unable to open CAN interface {}: {}",
                      CANOPEN_INTERFACE_NAME, error.message());
        return 1;
    }

    /* Remain false until the slave produces an accepted Boot callback after
     * master.Reset(); this gates every automatic process. */
    bool startup_boot_succeeded = false;
    /* The master owns local OD state, SDO/PDO services, NMT control, and all
     * callbacks used by the enabled A01-A05 processes. */
    lely::canopen::AsyncMaster master(
        executor, timer, channel, CANOPEN_MASTER_DCF_PATH, "",
        CANOPEN_MASTER_NODE_ID);
    master.OnCanState(canStateCallback);
    master.OnCanError(canErrorCallback);
    /* Register the shared remote NMT state observer before Reset can generate
     * state indications used by later confirmed NMT command waits. */
    registerNmtStateCallback(master);
    registerNmtHeartbeatCallbacks(master);

    /* Run the event loop on a dedicated thread so process functions can wait
     * synchronously for callbacks without blocking Lely itself. */
    std::thread canopen_thread(canopenWorker, std::ref(loop));

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

    /* Execute enabled automatic processes only after startup Boot proves the
     * remote node is reachable and managed by the current master instance. */
    if (startup_boot_succeeded) {
        error_count += canopenRunProcesses(master, g_canopen_processes.data(),
                                           g_canopen_processes.size());
    }

    /* Preserve the existing interactive lifetime: automatic validation ends
     * first, then the process remains available until Ctrl+C or loop failure. */
    spdlog::info(
        "Automatic CANopen tests finished; waiting for Ctrl+C");
    while (g_stop_requested == 0
           && g_canopen_loop_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (g_stop_requested == 0
        && !g_canopen_loop_running.load(std::memory_order_acquire)) {
        spdlog::error("CANopen event loop stopped before shutdown was requested");
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

    /* Cancel pending I/O before joining the event-loop thread. */
    context.shutdown();
    if (canopen_thread.joinable()) {
        canopen_thread.join();
    }

    spdlog::info("CANopen master exiting with result={}", error_count);
    spdlog::shutdown();
    return error_count == 0 ? 0 : 1;
}
