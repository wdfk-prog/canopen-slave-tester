/**
 * @file
 * @brief Implements shared EMCY event observation for automatic tests.
 */

#include "canopen_emcy.h"

#include <lely/coapp/master.hpp>

#include <spdlog/spdlog.h>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

namespace {

/** Bounded history prevents long interactive sessions from growing memory. */
constexpr std::size_t kEmcyEventCapacity = 32U;

/** Protects event publication, snapshots, and process-thread readers. */
std::mutex g_emcy_mutex;
/** Wakes process threads when the event-loop callback publishes a new EMCY. */
std::condition_variable g_emcy_condition;
/** Recent events retained for sequence-based waits and duplicate checks. */
std::deque<CanopenEmcyEvent> g_emcy_events;
/** Monotonic sequence; zero is reserved for the pre-event initial state. */
std::uint64_t g_emcy_sequence = 0;

/**
 * @brief Publish one EMCY received on the Lely event-loop thread.
 *
 * @param node_id Node that produced the EMCY message.
 * @param error_code EMCY error code.
 * @param error_register Remote error register value.
 * @param manufacturer_data Five manufacturer-specific EMCY bytes.
 */
void emcyCallback(std::uint8_t node_id, std::uint16_t error_code,
                  std::uint8_t error_register,
                  std::uint8_t manufacturer_data[5]) noexcept
{
    CanopenEmcyEvent event;
    event.node_id = node_id;
    event.error_code = error_code;
    event.error_register = error_register;
    for (std::size_t i = 0; i < event.manufacturer_data.size(); ++i) {
        event.manufacturer_data[i] = manufacturer_data[i];
    }
    event.timestamp = std::chrono::steady_clock::now();

    {
        /* Assign the sequence under the same lock as insertion so readers see
         * a total publication order across every EMCY callback. */
        std::lock_guard<std::mutex> lock(g_emcy_mutex);
        ++g_emcy_sequence;
        event.sequence = g_emcy_sequence;
        if (g_emcy_events.size() >= kEmcyEventCapacity) {
            g_emcy_events.pop_front();
        }
        g_emcy_events.push_back(event);
    }
    g_emcy_condition.notify_all();

    spdlog::info(
        "EMCY callback: seq={} node={} code=0x{:04x} "
        "error_register=0x{:02x} manufacturer={:02x} {:02x} {:02x} {:02x} "
        "{:02x}",
        event.sequence, static_cast<unsigned int>(node_id),
        static_cast<unsigned int>(error_code),
        static_cast<unsigned int>(error_register),
        static_cast<unsigned int>(manufacturer_data[0]),
        static_cast<unsigned int>(manufacturer_data[1]),
        static_cast<unsigned int>(manufacturer_data[2]),
        static_cast<unsigned int>(manufacturer_data[3]),
        static_cast<unsigned int>(manufacturer_data[4]));
}

/**
 * @brief Find the first matching event while the shared mutex is held.
 */
bool findEmcyEventLocked(std::uint64_t after_sequence, std::uint8_t node_id,
                         std::uint16_t error_code, CanopenEmcyEvent* event)
{
    for (const CanopenEmcyEvent& candidate : g_emcy_events) {
        if (candidate.sequence > after_sequence
            && candidate.node_id == node_id
            && candidate.error_code == error_code) {
            if (event != nullptr) {
                *event = candidate;
            }
            return true;
        }
    }
    return false;
}

} // namespace

void registerCanopenEmcyCallback(lely::canopen::AsyncMaster& master)
{
    /* OnEmcy has a single registration slot, so main.cpp installs this shared
     * observer once before startup Reset can generate remote events. */
    master.OnEmcy(emcyCallback);
}

std::uint64_t snapshotCanopenEmcySequence()
{
    std::lock_guard<std::mutex> lock(g_emcy_mutex);
    return g_emcy_sequence;
}

bool waitForCanopenEmcyEvent(std::uint64_t after_sequence,
                             std::uint8_t node_id,
                             std::uint16_t error_code,
                             std::chrono::milliseconds timeout,
                             CanopenEmcyEvent& event)
{
    std::unique_lock<std::mutex> lock(g_emcy_mutex);
    if (!g_emcy_condition.wait_for(lock, timeout, [&]() {
            return findEmcyEventLocked(after_sequence, node_id, error_code,
                                       nullptr);
        })) {
        return false;
    }

    return findEmcyEventLocked(after_sequence, node_id, error_code, &event);
}

std::vector<CanopenEmcyEvent> getCanopenEmcyEventsAfter(
    std::uint64_t after_sequence, std::uint8_t node_id)
{
    std::vector<CanopenEmcyEvent> events;
    std::lock_guard<std::mutex> lock(g_emcy_mutex);
    for (const CanopenEmcyEvent& event : g_emcy_events) {
        if (event.sequence > after_sequence && event.node_id == node_id) {
            events.push_back(event);
        }
    }
    return events;
}
