/**
 * @file
 * @brief Implements the final remote-node communication reset process.
 */

#include "shutdown_process.h"

#include "canopen_config.h"
#include "canopen_nmt.h"
#include "nmt_heartbeat.h"

#include <lely/coapp/master.hpp>

#include <spdlog/spdlog.h>

#include <chrono>

int finalResetProcess(lely::canopen::AsyncMaster& master)
{
    spdlog::info("Final reset communication process started");

    /* Step 1: clear the previous Boot result before the reset command can
     * produce a new callback on the event-loop thread. */
    prepareBootWait();

    /* Step 2: reset only the configured slave communication profile; this is
     * intentionally narrower than a full node reset. */
    if (!issueNmtCommand(master, lely::canopen::NmtCommand::RESET_COMM,
                         CANOPEN_SLAVE_NODE_ID,
                         "Final slave Reset Communication")) {
        return 1;
    }

    /* Step 3: require a fresh Boot callback so shutdown does not silently
     * leave the slave offline. */
    if (!waitForBootCompletion(
            std::chrono::milliseconds(CANOPEN_WAIT_TIMEOUT_MS))) {
        spdlog::error(
            "Remote node did not complete Boot after final reset");
        return 1;
    }

    spdlog::info("Final reset communication process completed");
    return 0;
}
