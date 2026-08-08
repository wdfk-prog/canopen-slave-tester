/**
 * @file
 * @brief Implements CANopen master initialization and process orchestration.
 */

#include "canopen_config.h"
#include "canopen_process.h"
#include "nmt_heartbeat.h"
#include "sdo_process.h"
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
const std::array<CanopenProcessEntry,
                 CANOPEN_ENABLE_HEARTBEAT_PROCESS
                     + CANOPEN_ENABLE_SDO_PROCESS>
    g_canopen_processes = {{
#if CANOPEN_ENABLE_HEARTBEAT_PROCESS
        {"A01 Heartbeat", heartbeatProcess},
#endif /* CANOPEN_ENABLE_HEARTBEAT_PROCESS */
#if CANOPEN_ENABLE_SDO_PROCESS
        {"A02 SDO", sdoProcess},
#endif /* CANOPEN_ENABLE_SDO_PROCESS */
    }};

volatile std::sig_atomic_t g_stop_requested = 0;
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
    spdlog::init_thread_pool(CANOPEN_LOG_QUEUE_SIZE,
                             CANOPEN_LOG_WORKER_COUNT);
    const auto sink =
        std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
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
    g_canopen_loop_running.store(true, std::memory_order_release);

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
    int error_count = 0;
    initializeLogging();

    if (std::signal(SIGINT, signalHandler) == SIG_ERR
        || std::signal(SIGTERM, signalHandler) == SIG_ERR) {
        spdlog::error("Unable to install process signal handlers");
        return 1;
    }

    lely::io::IoGuard io_guard;
    lely::io::Context context;
    lely::io::Poll poll(context);
    lely::ev::Loop loop(poll.get_poll());
    lely::ev::Executor executor = loop.get_executor();
    lely::io::Timer timer(poll, executor, CLOCK_MONOTONIC);
    lely::io::CanController controller(CANOPEN_INTERFACE_NAME);

    std::error_code error;
    int nominal_bitrate = 0;
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

    lely::io::CanChannel channel(poll, executor,
                                 CANOPEN_CHANNEL_RX_QUEUE_SIZE);
    channel.open(controller, lely::io::CanBusFlag::NONE, error);
    if (error) {
        spdlog::error("Unable to open CAN interface {}: {}",
                      CANOPEN_INTERFACE_NAME, error.message());
        return 1;
    }

    bool startup_boot_succeeded = false;
    lely::canopen::AsyncMaster master(
        executor, timer, channel, CANOPEN_MASTER_DCF_PATH, "",
        CANOPEN_MASTER_NODE_ID);
    master.OnCanState(canStateCallback);
    master.OnCanError(canErrorCallback);
    registerNmtHeartbeatCallbacks(master);

    std::thread canopen_thread(canopenWorker, std::ref(loop));

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

    if (startup_boot_succeeded) {
        error_count += canopenRunProcesses(master, g_canopen_processes.data(),
                                           g_canopen_processes.size());
    }

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
    if (g_canopen_loop_running.load(std::memory_order_acquire)
        && startup_boot_succeeded) {
        error_count += finalResetProcess(master);
    }
#endif /* CANOPEN_ENABLE_FINAL_RESET_PROCESS */

    context.shutdown();
    if (canopen_thread.joinable()) {
        canopen_thread.join();
    }

    spdlog::info("CANopen master exiting with result={}", error_count);
    spdlog::shutdown();
    return error_count == 0 ? 0 : 1;
}
