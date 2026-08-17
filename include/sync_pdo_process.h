/**
 * @file
 * @brief SYNC consumer and synchronous TPDO validation interface.
 */

#ifndef SYNC_PDO_PROCESS_H
#define SYNC_PDO_PROCESS_H

/** Enable the A04 SYNC consumer and synchronous TPDO verification process. */
#define CANOPEN_ENABLE_SYNC_PDO_PROCESS 0

namespace lely {
namespace canopen {
class AsyncMaster;
} // namespace canopen
} // namespace lely

/**
 * @brief Verify slave SYNC consumption and synchronous TPDO1 behavior.
 *
 * The process requires the remote node to remain a SYNC consumer. It
 * temporarily changes TPDO1 to synchronous cyclic transmission type 1, uses
 * the local Lely master as the only SYNC producer, validates one TPDO1 per
 * SYNC, and restores the original TPDO1 communication parameters.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @return Zero on success; otherwise a non-zero process result.
 */
int syncPdoProcess(lely::canopen::AsyncMaster& master);

#endif /* SYNC_PDO_PROCESS_H */
