/**
 * @file
 * @brief Boot, Heartbeat, and related EMCY test interfaces.
 */

#ifndef NMT_HEARTBEAT_H
#define NMT_HEARTBEAT_H

/** Enable the A01 bidirectional Heartbeat verification process. */
#define CANOPEN_ENABLE_HEARTBEAT_PROCESS 1

#include <chrono>

namespace lely {
namespace canopen {
class AsyncMaster;
} // namespace canopen
} // namespace lely

/**
 * @brief Register callbacks used by the bidirectional Heartbeat test.
 *
 * @param master Lely asynchronous master that owns the callback hooks.
 */
void registerNmtHeartbeatCallbacks(lely::canopen::AsyncMaster& master);

/**
 * @brief Clear the previous Boot result before an expected Boot procedure.
 */
void prepareBootWait();

/**
 * @brief Wait for a successful Boot callback from the configured slave.
 *
 * Boot status zero and status 'L' are accepted.
 *
 * @param timeout Maximum wait duration.
 * @return true when an accepted Boot result is received; otherwise false.
 */
bool waitForBootCompletion(std::chrono::milliseconds timeout);

/**
 * @brief Exercise both directions of Producer Heartbeat supervision.
 *
 * The slave Heartbeat objects are configured by Lely Boot from the generated
 * concise DCF. The process first interrupts the local master Producer
 * Heartbeat and verifies slave EMCY timeout/recovery, then uses remote SDO
 * downloads to stop and restore the slave Producer Heartbeat and verifies the
 * master Heartbeat timeout/recovery callbacks.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @return Zero on success; otherwise a non-zero process result.
 */
int heartbeatProcess(lely::canopen::AsyncMaster& master);

#endif /* NMT_HEARTBEAT_H */
