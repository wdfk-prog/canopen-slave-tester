/**
 * @file
 * @brief J04/B03 MCU SDO Client validation interface.
 */

#ifndef SDO_CLIENT_PROCESS_H
#define SDO_CLIENT_PROCESS_H

/** Enable J04/B03 MCU SDO Client validation. */
#define CANOPEN_ENABLE_SDO_CLIENT_PROCESS 1

class EmcyTestMaster;

/**
 * @brief Validate MCU local/remote SDO Client transactions and recovery.
 *
 * @param master Active Host master used both for MCU control SDOs and the
 *        temporary Node-127 Server-SDO fixture.
 * @return Zero on success; otherwise a non-zero process result.
 */
int sdoClientProcess(EmcyTestMaster& master);

#endif /* SDO_CLIENT_PROCESS_H */
