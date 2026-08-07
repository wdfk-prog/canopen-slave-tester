/**
 * @file
 * @brief Implements the final remote-node communication reset process.
 */

#include "shutdown_process.h"

#include "canopen_config.h"
#include "nmt_heartbeat.h"

#include <lely/coapp/master.hpp>

#include <spdlog/spdlog.h>

#include <chrono>

int finalResetProcess(lely::canopen::AsyncMaster& master)
{
    spdlog::info("Final reset communication process started");

    /* Clear the previous Boot result before the reset command can produce a
     * new callback on the event-loop thread. */
    prepareBootWait();
    master.Command(lely::canopen::NmtCommand::RESET_COMM,
                   CANOPEN_SLAVE_NODE_ID);

    if (!waitForBootCompletion(
            std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS))) {
        spdlog::error(
            "Remote node did not complete Boot after final reset");
        return 1;
    }

    spdlog::info("Final reset communication process completed");
    return 0;
}
