/**
 * @file
 * @brief SDO server block-transfer validation interface.
 */

#ifndef SDO_BLOCK_PROCESS_H
#define SDO_BLOCK_PROCESS_H

/** Enable SDO server block-transfer validation. */
#define CANOPEN_ENABLE_SDO_BLOCK_PROCESS 0

class CanopenTestMaster;

/**
 * @brief Validate explicit SDO block transfer, abort handling and recovery.
 *
 * @param master Active Host master used to access the MCU SDO server.
 * @return Zero on success; otherwise a non-zero process result.
 */
int sdoBlockProcess(CanopenTestMaster& master);

#endif /* SDO_BLOCK_PROCESS_H */
