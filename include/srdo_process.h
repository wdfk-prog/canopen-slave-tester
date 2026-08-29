/**
 * @file
 * @brief J09/B09S SRDO protocol validation interface.
 */

#ifndef SRDO_PROCESS_H
#define SRDO_PROCESS_H

/** Enable J09/B09S Safety-Related Data Object protocol validation. */
#define CANOPEN_ENABLE_SRDO_PROCESS 1

namespace lely {
namespace io {
class CanChannel;
} // namespace io
} // namespace lely

class CanopenTestMaster;

/**
 * @brief Validate MCU CANopenNode SRDO protocol behavior.
 *
 * The regular Lely master remains responsible for SDO/NMT/Boot control. A
 * dedicated safety-wire CanChannel on the same SocketCAN interface is used
 * exclusively by this SRDO stage while it runs and handles only the fixed
 * SRDO test-profile CAN-IDs 0x101..0x104.
 *
 * @param master Active Host CANopen master used for SDO/NMT control.
 * @param wire_channel Sequential safety-wire channel owned by this SRDO stage while it runs.
 * @return Zero on success; otherwise a non-zero process result.
 */
int srdoProcess(CanopenTestMaster& master, lely::io::CanChannel& wire_channel);

#endif /* SRDO_PROCESS_H */
