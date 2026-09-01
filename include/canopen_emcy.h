/**
 * @file
 * @brief Shared EMCY event observation for automatic CANopen tests.
 */

#ifndef CANOPEN_EMCY_H
#define CANOPEN_EMCY_H

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

namespace lely {
namespace canopen {
class AsyncMaster;
} // namespace canopen
} // namespace lely

/** One EMCY event published by the shared Lely callback. */
struct CanopenEmcyEvent {
    /** Monotonic event sequence used to reject callbacks from earlier steps. */
    std::uint64_t sequence = 0;
    /** Node-ID that produced the EMCY frame. */
    std::uint8_t node_id = 0;
    /** CiA 301 emergency error code. */
    std::uint16_t error_code = 0;
    /** Error register value carried in the EMCY frame. */
    std::uint8_t error_register = 0;
    /** Five manufacturer-specific EMCY bytes as delivered by Lely. */
    std::array<std::uint8_t, 5> manufacturer_data{{0, 0, 0, 0, 0}};
    /** Monotonic receive time used by timing assertions such as 0x1015. */
    std::chrono::steady_clock::time_point timestamp{};
};

/**
 * @brief Register the single shared EMCY callback on the Lely master.
 *
 * A Lely master has one OnEmcy() callback slot. Heartbeat validation, EMCY producer validation, and future tests
 * therefore consume events from this shared observer instead of replacing one
 * another's callback registration.
 *
 * @param master Active Lely asynchronous CANopen master.
 */
void registerCanopenEmcyCallback(lely::canopen::AsyncMaster& master);

/**
 * @brief Snapshot the latest published EMCY sequence.
 *
 * @return Sequence of the newest event, or zero before the first event.
 */
std::uint64_t snapshotCanopenEmcySequence();

/**
 * @brief Wait for a matching EMCY published after a sequence snapshot.
 *
 * @param after_sequence Ignore events at or before this sequence.
 * @param node_id Required producer node-ID.
 * @param error_code Required EMCY error code.
 * @param timeout Maximum wait duration.
 * @param event Receives the first matching event.
 * @return true when a matching event is observed; otherwise false.
 */
bool waitForCanopenEmcyEvent(std::uint64_t after_sequence,
                             std::uint8_t node_id,
                             std::uint16_t error_code,
                             std::chrono::milliseconds timeout,
                             CanopenEmcyEvent& event);

/**
 * @brief Copy EMCY events for one node published after a sequence snapshot.
 *
 * @param after_sequence Ignore events at or before this sequence.
 * @param node_id Producer node-ID to select.
 * @return Matching events in callback publication order.
 */
std::vector<CanopenEmcyEvent> getCanopenEmcyEventsAfter(
    std::uint64_t after_sequence, std::uint8_t node_id);

#endif /* CANOPEN_EMCY_H */
