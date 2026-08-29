/**
 * @file
 * @brief HDR Filter Stage-1 host validation process.
 */

#ifndef HDR_FILTER_PROCESS_H
#define HDR_FILTER_PROCESS_H

/** Enable the H00-H08 HDR Filter Stage-1 validation process. */
#define CANOPEN_ENABLE_HDR_FILTER_PROCESS 0

/**
 * Enable destructive H06-A Reset Communication double-failure validation.
 *
 * The expected result is that the DUT cannot return to CAN normal mode. This
 * case is disabled by default because the next test requires a hardware reset
 * or power cycle of the DUT.
 */
#define CANOPEN_HDR_FILTER_RUN_DESTRUCTIVE_RESET_FAIL_TEST 0

class CanopenTestMaster;

namespace lely {
namespace io {
class CanChannel;
} // namespace io
} // namespace lely

/**
 * @brief Run HDR Filter Stage-1 host validation against the MCU demo OD.
 *
 * H09 runtime LSS bitrate switching remains a conditional target/HIL case
 * because the host must also reconfigure SocketCAN during the CiA 305 silence
 * window. The DUT-side filter-generation diagnostics used to prove H09 are
 * implemented by the companion canopennode-rtt change.
 *
 * @param master Active CANopen test master.
 * @param wire_channel Dedicated raw CAN channel used for DTR/RTR injection.
 * @return Zero on success; otherwise non-zero.
 */
int hdrFilterProcess(CanopenTestMaster& master,
                     lely::io::CanChannel& wire_channel);

#endif /* HDR_FILTER_PROCESS_H */
