/**
 * @file
 * @brief User object dictionary SDO test interface.
 */

#ifndef SDO_PROCESS_H
#define SDO_PROCESS_H

/** Enable SDO object-access validation for the user Object Dictionary. */
#define CANOPEN_ENABLE_SDO_PROCESS 0

namespace lely {
namespace canopen {
class AsyncMaster;
} // namespace canopen
} // namespace lely

/**
 * @brief Verify SDO upload/download access to the configured user OD entry.
 *
 * The process saves 0x2200:00, writes a temporary value, verifies it by SDO
 * upload, then restores and verifies the original value.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @return Zero on success; otherwise a non-zero process result.
 */
int sdoProcess(lely::canopen::AsyncMaster& master);

#endif /* SDO_PROCESS_H */
