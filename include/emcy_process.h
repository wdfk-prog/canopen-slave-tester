/**
 * @file
 * @brief A06 EMCY producer validation interface.
 */

#ifndef EMCY_PROCESS_H
#define EMCY_PROCESS_H

/** Enable the A06 EMCY producer validation process. */
#define CANOPEN_ENABLE_EMCY_PROCESS 0

namespace lely {
namespace canopen {
class AsyncMaster;
} // namespace canopen
} // namespace lely

/**
 * @brief Verify EMCY producer state, history, configurable COB-ID, and inhibit.
 *
 * Heartbeat consumer timeout remains an A01 capability test. A06 only reuses
 * loss of the master's Producer Heartbeat as a deterministic EMCY fault source
 * and validates the resulting EMCY producer behavior through 0x1001, 0x1003,
 * 0x1014, and 0x1015. All modified communication values are restored and
 * verified before the process returns.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @return Zero on success; otherwise a non-zero process result.
 */
int emcyProcess(lely::canopen::AsyncMaster& master);

#endif /* EMCY_PROCESS_H */
