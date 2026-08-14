/**
 * @file
 * @brief NMT master behavior validation interface for the Lely slave role.
 */

#ifndef NMT_MASTER_PROCESS_H
#define NMT_MASTER_PROCESS_H

/** Enable NMT master behavior validation in the Lely slave role. */
#define CANOPEN_ENABLE_NMT_MASTER_PROCESS 1
/** Maximum wait for each expected NMT command transition, in ms. */
#define CANOPEN_NMT_MASTER_STEP_TIMEOUT_MS 5000

namespace lely {
namespace canopen {
class BasicSlave;
} // namespace canopen
} // namespace lely

/**
 * @brief Validate NMT commands produced by an external CANopen master.
 *
 * The Lely BasicSlave acts as the controlled remote node. The process observes
 * BasicSlave::OnCommand() and verifies the command sequence required by the MCU
 * NMT-master test contract. The Host leaves the MCU-provided EDS unchanged and
 * reads local 0x1F80 only to determine the reset startup branch. If the software
 * peer auto-starts Operational, the MCU first sends an extra PREOP command to
 * normalize the fixture before the six formal validation commands.
 * The same normalization is repeated after resets when Lely automatically
 * starts the peer. Producer Heartbeat is restored only after reset completion
 * reaches PRE-OP. After the final START callback, the peer remains alive through
 * two full Producer Heartbeat periods for the MCU final-state assertion.
 *
 * @pre BasicSlave::Reset() has completed. This function registers the command
 *      observer before enabling Producer Heartbeat, so fixture preparation and
 *      all formal validation commands are captured without changing the EDS.
 *
 * @param slave Active Lely BasicSlave used as the controlled CANopen node.
 * @return Zero when the complete command sequence is observed; otherwise a
 *         non-zero process result.
 */
int nmtMasterProcess(lely::canopen::BasicSlave& slave);

#endif /* NMT_MASTER_PROCESS_H */
