/**
 * @file
 * @brief Passive NMT transition callback tracing for a Lely BasicSlave test peer.
 */

#ifndef SLAVE_PEER_TRACE_H
#define SLAVE_PEER_TRACE_H

namespace lely {
namespace canopen {
class BasicSlave;
} // namespace canopen
} // namespace lely

/**
 * @brief Register a non-asserting OnCommand callback on a BasicSlave peer.
 *
 * The callback records BasicSlave OnCommand transition callbacks but deliberately
 * does not treat them as wire-level proof of a received NMT command. This keeps
 * the fixture compatible with the Lely NMT boot sequence driven by
 * lely-canopen-rtt. This helper owns the
 * single BasicSlave OnCommand callback slot until
 * uninstallSlavePeerNmtTrace() is called.
 *
 * @param slave Active software CANopen slave peer.
 */
void installSlavePeerNmtTrace(lely::canopen::BasicSlave& slave);

/**
 * @brief Remove the passive NMT transition callback from a BasicSlave peer.
 *
 * Call this only while the passive trace owns the OnCommand callback slot;
 * the operation intentionally clears that slot instead of restoring an
 * unrelated callback.
 *
 * @param slave Active software CANopen slave peer.
 */
void uninstallSlavePeerNmtTrace(lely::canopen::BasicSlave& slave);

#endif /* SLAVE_PEER_TRACE_H */
