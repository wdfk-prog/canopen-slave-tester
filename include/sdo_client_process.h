/**
 * @file
 * @brief J04/B03 MCU SDO Client validation interface.
 */

#ifndef SDO_CLIENT_PROCESS_H
#define SDO_CLIENT_PROCESS_H

/** Enable J04/B03 MCU SDO Client validation. */
#define CANOPEN_ENABLE_SDO_CLIENT_PROCESS 0
/** Enable optional J06/B02-12 MCU SDO Client block-transfer regression. */
#ifndef CANOPEN_ENABLE_SDO_CLIENT_BLOCK_REGRESSION
#define CANOPEN_ENABLE_SDO_CLIENT_BLOCK_REGRESSION 0
#endif /* CANOPEN_ENABLE_SDO_CLIENT_BLOCK_REGRESSION */

#if CANOPEN_ENABLE_SDO_CLIENT_BLOCK_REGRESSION && !CANOPEN_ENABLE_SDO_CLIENT_PROCESS
#error "B02-12 requires CANOPEN_ENABLE_SDO_CLIENT_PROCESS"
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
