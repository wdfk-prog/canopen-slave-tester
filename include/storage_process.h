/**
 * @file
 * @brief CANopenNode EEPROM storage persistence validation interface.
 */

#ifndef STORAGE_PROCESS_H
#define STORAGE_PROCESS_H

/** Enable CANopenNode EEPROM storage persistence validation. */
#define CANOPEN_ENABLE_STORAGE_PROCESS 0
/** Environment variable selecting one destructive Storage persistence validation execution group. */
#define CANOPEN_STORAGE_MODE_ENV "CANOPEN_STORAGE_MODE"
/** Maximum wait for an operator-driven power event and fresh Boot callback. */
#define CANOPEN_STORAGE_OPERATOR_TIMEOUT_MS 60000

namespace lely {
namespace canopen {
class AsyncMaster;
} // namespace canopen
} // namespace lely

/**
 * @brief Run one Storage persistence validation mode selected by CANOPEN_STORAGE_MODE.
 *
 * Supported modes are core, restore, corruption, power-cycle and
 * power-interruption. Missing CANOPEN_STORAGE_MODE defaults to core. Every
 * destructive mode captures the complete raw Storage entry baseline before
 * modification and verifies full restoration before reporting success.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @return Zero on success; otherwise a non-zero process result.
 */
int storageProcess(lely::canopen::AsyncMaster& master);

#endif /* STORAGE_PROCESS_H */
