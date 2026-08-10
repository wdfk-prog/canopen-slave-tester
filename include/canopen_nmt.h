/**
 * @file
 * @brief Common NMT command helper for CANopen test processes.
 */

#ifndef CANOPEN_NMT_H
#define CANOPEN_NMT_H

#include <lely/coapp/node.hpp>

#include <chrono>
#include <cstdint>

namespace lely {
namespace canopen {
class AsyncMaster;
} // namespace canopen
} // namespace lely

/**
 * @brief Issue one NMT command and convert Lely exceptions to failure status.
 *
 * A successful return means the local Lely master accepted the command
 * request. It does not prove that the remote node has completed the requested
 * state transition; callers must wait for Boot/NMT/Heartbeat evidence when
 * such confirmation is required.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param command NMT command to issue.
 * @param node_id Target node-ID, or zero when a caller intentionally broadcasts.
 * @param description Human-readable operation text used in failure diagnostics.
 * @return true when Lely accepts the command; otherwise false.
 */
bool issueNmtCommand(lely::canopen::AsyncMaster& master,
                     lely::canopen::NmtCommand command,
                     std::uint8_t node_id, const char* description);

/**
 * @brief Register the remote NMT state observer used by confirmed command waits.
 *
 * This function owns the single AsyncMaster::OnState() callback slot and must
 * therefore be called once during master initialization before any call to
 * issueNmtCommandAndWaitForState(). The observer tracks only remote node state
 * indications reported by Lely's heartbeat/NMT state callback.
 *
 * @param master Lely asynchronous master that owns the OnState callback.
 */
void registerNmtStateCallback(lely::canopen::AsyncMaster& master);

/**
 * @brief Issue one remote NMT command and wait for a fresh target-state event.
 *
 * The wait starts from the state-event generation already published before
 * Command() is submitted, so an already-cached stale state cannot satisfy the
 * request. A successful return requires local command acceptance and a later
 * publication of the requested OnState() value. Lely does not attach a command
 * token to OnState(), so this helper provides event-ordering evidence rather
 * than strict transaction correlation. An older callback that entered dispatch
 * before Command() but was delayed before publishing its state is a theoretical
 * residual race accepted by this project. This helper is intended for nonzero
 * remote node-IDs; broadcasts and local-master state changes cannot be confirmed
 * through the remote OnState callback. Because it blocks waiting for that
 * callback, it must not be called from the Lely event-loop thread that dispatches
 * OnState().
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param command NMT command to issue.
 * @param node_id Nonzero remote node-ID whose state must be confirmed.
 * @param expected_state Remote NMT state required after the command.
 * @param timeout Maximum wait for a fresh matching OnState indication.
 * @param description Human-readable operation text used in diagnostics.
 * @return true when the command is accepted and the fresh target state is
 *         observed; otherwise false.
 */
bool issueNmtCommandAndWaitForState(
    lely::canopen::AsyncMaster& master, lely::canopen::NmtCommand command,
    std::uint8_t node_id, lely::canopen::NmtState expected_state,
    std::chrono::milliseconds timeout, const char* description);

#endif /* CANOPEN_NMT_H */
