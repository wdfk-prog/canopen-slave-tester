/**
 * @file
 * @brief MCU SDO client validation interface.
 */

#ifndef SDO_CLIENT_PROCESS_H
#define SDO_CLIENT_PROCESS_H

/** Enable MCU SDO client validation. */
#define CANOPEN_ENABLE_SDO_CLIENT_PROCESS 0
/** Enable optional MCU SDO client block-transfer regression. */
#ifndef CANOPEN_ENABLE_SDO_CLIENT_BLOCK_REGRESSION
#define CANOPEN_ENABLE_SDO_CLIENT_BLOCK_REGRESSION 0
#endif /* CANOPEN_ENABLE_SDO_CLIENT_BLOCK_REGRESSION */

#if CANOPEN_ENABLE_SDO_CLIENT_BLOCK_REGRESSION && !CANOPEN_ENABLE_SDO_CLIENT_PROCESS
#error "MCU SDO client block-transfer regression requires CANOPEN_ENABLE_SDO_CLIENT_PROCESS"
#endif /* CANOPEN_ENABLE_SDO_CLIENT_BLOCK_REGRESSION && !CANOPEN_ENABLE_SDO_CLIENT_PROCESS */

class CanopenTestMaster;

/**
 * @brief Validate MCU local/remote SDO Client transactions and recovery.
 *
 * @param master Active Host master used both for MCU control SDOs and the
 *        temporary Node-127 Server-SDO fixture.
 * @return Zero on success; otherwise a non-zero process result.
 */
int sdoClientProcess(CanopenTestMaster& master);

#endif /* SDO_CLIENT_PROCESS_H */
