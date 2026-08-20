/**
 * @file
 * @brief B06 EMCY consumer validation interface.
 */

#ifndef EMCY_CONSUMER_PROCESS_H
#define EMCY_CONSUMER_PROCESS_H

/** Enable the B06 MCU EMCY consumer validation process. */
#define CANOPEN_ENABLE_EMCY_CONSUMER_PROCESS 0

class CanopenTestMaster;

/**
 * @brief Verify the MCU EMCY consumer and diagnostic delivery path.
 *
 * The Host emits deterministic EMCY messages from its existing Lely master
 * node and reads the MCU demo diagnostic record at 0x2301 through SDO. The
 * process checks message fields, duplicate delivery, recovery EMCY, ordinary
 * SDO health, and callback re-registration after Reset Communication.
 *
 * @param master B06 test master with access to Lely local EMCY recovery.
 * @return Zero on success; otherwise a non-zero process result.
 */
int emcyConsumerProcess(CanopenTestMaster& master);

#endif /* EMCY_CONSUMER_PROCESS_H */
