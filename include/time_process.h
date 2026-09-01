/**
 * @file
 * @brief TIME consumer validation interface.
 */

#ifndef TIME_PROCESS_H
#define TIME_PROCESS_H

/** Enable TIME consumer validation after the MCU exposes the required 0x2300 diagnostic record. */
#define CANOPEN_ENABLE_TIME_PROCESS 0

namespace lely {
namespace canopen {
class AsyncMaster;
} // namespace canopen
} // namespace lely

/**
 * @brief Verify slave TIME consumption through observable diagnostic OD data.
 *
 * The process enables the slave TIME consumer when required, injects controlled
 * TIME frames through the existing Lely CAN network, verifies the applied TIME
 * diagnostic object, exercises valid/boundary/invalid-DLC and disable/re-enable
 * behavior, and restores the original 0x1012 value.
 *
 * The MCU test firmware must expose the diagnostic record described by TIME consumer validation at
 * 0x2300:01..03. The receive-count field is expected to increment from the MCU
 * TIME receive callback only for syntactically valid DLC=6 TIME frames.
 * Application is verified independently through the diagnostic ms/day fields.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @return Zero on success; otherwise a non-zero process result.
 */
int timeProcess(lely::canopen::AsyncMaster& master);

#endif /* TIME_PROCESS_H */
