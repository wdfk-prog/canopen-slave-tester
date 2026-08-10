/**
 * @file
 * @brief RPDO and TPDO validation process interface.
 */

#ifndef PDO_PROCESS_H
#define PDO_PROCESS_H

/** Enable the A03 RPDO/TPDO verification process. */
#define CANOPEN_ENABLE_PDO_PROCESS 1

namespace lely {
namespace canopen {
class AsyncMaster;
} // namespace canopen
} // namespace lely

/**
 * @brief Verify TPDO reception and RPDO delivery for the configured slave.
 *
 * The process checks the default TPDO1 mapping and period, compares TPDO data
 * with the mapped values and remote OD, sends an RPDO1 probe value through the
 * Lely PDO API, then restores and verifies the original 0x2200:00 value.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @return Zero on success; otherwise a non-zero process result.
 */
int pdoProcess(lely::canopen::AsyncMaster& master);

#endif /* PDO_PROCESS_H */
