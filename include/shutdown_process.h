/**
 * @file
 * @brief Final remote-node reset process interface.
 */

#ifndef SHUTDOWN_PROCESS_H
#define SHUTDOWN_PROCESS_H

namespace lely {
namespace canopen {
class AsyncMaster;
} // namespace canopen
} // namespace lely

/**
 * @brief Reset remote communication and wait for the following Boot callback.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @return Zero on success; otherwise a non-zero process result.
 */
int finalResetProcess(lely::canopen::AsyncMaster& master);

#endif /* SHUTDOWN_PROCESS_H */
