/**
 * @file
 * @brief Boot and bidirectional Heartbeat test interfaces.
 */

#ifndef NMT_HEARTBEAT_H
#define NMT_HEARTBEAT_H

/** Enable bidirectional Heartbeat producer/consumer validation. */
#define CANOPEN_ENABLE_HEARTBEAT_PROCESS 0

#include <chrono>

namespace lely {
namespace canopen {
class AsyncMaster;
enum class NmtState;
} // namespace canopen
} // namespace lely

/**
 * @brief Register Boot and Heartbeat callbacks used by Heartbeat validation.
 *
 * EMCY observation is registered separately through the shared observer so
 * Heartbeat validation and EMCY producer validation do not compete for Lely's single OnEmcy() callback slot.
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
 * @brief Wait for a successful Boot callback and return its reported NMT state.
 *
 * Boot status zero and status 'L' are accepted. The output state is updated only
 * when an accepted Boot result is received.
 *
 * @param timeout Maximum wait duration.
 * @param state NMT state reported by the accepted Boot callback.
 * @return true when an accepted Boot result is received; otherwise false.
 */
bool waitForBootCompletion(std::chrono::milliseconds timeout,
                           lely::canopen::NmtState& state);

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
