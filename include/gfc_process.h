/**
 * @file
 * @brief GFC protocol validation interface.
 */

#ifndef GFC_PROCESS_H
#define GFC_PROCESS_H

/** Enable GFC (Global Fail-safe Command) protocol validation. */
#define CANOPEN_ENABLE_GFC_PROCESS 0

namespace lely {
namespace io {
class CanChannel;
} // namespace io
} // namespace lely

class CanopenTestMaster;

/**
 * @brief Validate MCU GFC consumer/producer behavior with a dedicated wire channel.
 *
 * The regular Lely master remains responsible for SDO/NMT control. A second
 * CanChannel on the same SocketCAN interface is used only for fixed GFC wire
 * injection and capture on CAN-ID 0x001.
 *
 * @param master Active Host CANopen master used for SDO/NMT control.
 * @param wire_channel Dedicated CAN channel used only by the GFC wire fixture.
 * @return Zero on success; otherwise a non-zero process result.
 */
int gfcProcess(CanopenTestMaster& master, lely::io::CanChannel& wire_channel);

#endif /* GFC_PROCESS_H */
