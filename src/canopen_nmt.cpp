/**
 * @file
 * @brief Implements the common NMT command helper.
 */

#include "canopen_nmt.h"

#include <lely/coapp/master.hpp>

#include <spdlog/spdlog.h>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <system_error>

namespace {

/** Number of addressable CANopen node-ID values including index zero. */
constexpr std::size_t kNmtStateSlotCount = 128U;
/** Lely local Request NMT object containing the current managed-node state. */
constexpr std::uint16_t kRequestNmtIndex = 0x1F82U;

/** Latest remote NMT state observation for one node-ID. */
struct NmtStateObservation {
    std::uint64_t generation = 0; /**< Incremented for every remote state event. */
    lely::canopen::NmtState state = lely::canopen::NmtState::BOOTUP; /**< Last observed state placeholder. */
    bool observed = false; /**< false until at least one OnState event arrives. */
};

/** Protects the shared state table and registered-master identity. */
std::mutex g_nmt_state_mutex;
/** Wakes command waiters after any remote NMT state event. */
std::condition_variable g_nmt_state_condition;
/** Per-node remote state cache indexed directly by node-ID. */
std::array<NmtStateObservation, kNmtStateSlotCount> g_nmt_states{};
/** Master instance whose single OnState callback slot is owned by this module. */
lely::canopen::AsyncMaster* g_registered_master = nullptr;

/**
 * @brief Publish one remote NMT state indication from the Lely event loop.
 *
 * @param node_id Remote node-ID reported by Lely.
 * @param state Newly observed remote NMT state.
 */
void nmtStateCallback(std::uint8_t node_id,
                      lely::canopen::NmtState state) noexcept
{
    if (node_id == 0U || node_id >= kNmtStateSlotCount) {
        return;
    }

    {
        /* Publish state and generation atomically before waking waiters. */
        std::lock_guard<std::mutex> lock(g_nmt_state_mutex);
        NmtStateObservation& observation = g_nmt_states[node_id];
        ++observation.generation;
        observation.state = state;
        observation.observed = true;
    }
    g_nmt_state_condition.notify_all();
}

/**
 * @brief Read Lely's current managed state for one remote node.
 *
 * The local 0x1F82 Request NMT entry is updated by Lely during Boot and later
 * NMT state indications, so it can confirm an idempotent command even when no
 * new OnState callback is generated for an unchanged state.
 */
bool readManagedNmtState(lely::canopen::AsyncMaster& master, std::uint8_t node_id,
                         lely::canopen::NmtState& state)
{
    std::error_code error;
    const std::uint8_t raw_state = master.Read<std::uint8_t>(kRequestNmtIndex, node_id, error);
    if (error) {
        return false;
    }

    state = static_cast<lely::canopen::NmtState>(raw_state);
    return true;
}

} // namespace

bool issueNmtCommand(lely::canopen::AsyncMaster& master,
                     lely::canopen::NmtCommand command,
                     std::uint8_t node_id, const char* description)
{
    try {
        master.Command(command, node_id);
    } catch (const std::exception& exception) {
        spdlog::error("{} failed: {}", description, exception.what());
        return false;
    }
    return true;
}

void registerNmtStateCallback(lely::canopen::AsyncMaster& master)
{
    /* Install the one shared remote-state observer before any NMT command wait. */
    master.OnState(nmtStateCallback);

    /* A new master instance starts with no accepted cached remote state. */
    std::lock_guard<std::mutex> lock(g_nmt_state_mutex);
    g_nmt_states = {};
    g_registered_master = &master;
}

bool issueNmtCommandAndWaitForState(
    lely::canopen::AsyncMaster& master, lely::canopen::NmtCommand command,
    std::uint8_t node_id, lely::canopen::NmtState expected_state,
    std::chrono::milliseconds timeout, const char* description)
{
    if (node_id == 0U || node_id >= kNmtStateSlotCount) {
        spdlog::error(
            "{} cannot confirm invalid/broadcast node-ID {}", description,
            static_cast<unsigned int>(node_id));
        return false;
    }

    /* Snapshot Lely's managed state without holding the observer mutex. Device
     * access may take Lely-internal locks, while the observer mutex is only for
     * the test-side generation cache. */
    lely::canopen::NmtState managed_state_before = lely::canopen::NmtState::BOOTUP;
    const bool already_expected_before =
        readManagedNmtState(master, node_id, managed_state_before)
        && managed_state_before == expected_state;

    /* Hold the observation mutex across the generation snapshot and Command() so
     * no OnState callback can publish between those two operations. */
    std::uint64_t baseline_generation = 0;
    {
        std::lock_guard<std::mutex> lock(g_nmt_state_mutex);
        if (g_registered_master != &master) {
            spdlog::error(
                "{} cannot wait for NMT state before state callback registration",
                description);
            return false;
        }

        baseline_generation = g_nmt_states[node_id].generation;
        if (!issueNmtCommand(master, command, node_id, description)) {
            return false;
        }
    }

    /* Re-read after command acceptance before using the idempotent shortcut.
     * This prevents a stale pre-command 0x1F82 value from satisfying the wait if
     * the managed node changed state while the command was being submitted. */
    if (already_expected_before) {
        lely::canopen::NmtState managed_state_after = lely::canopen::NmtState::BOOTUP;
        if (readManagedNmtState(master, node_id, managed_state_after)
            && managed_state_after == expected_state) {
            spdlog::debug(
                "{} accepted as idempotent: node={} already state=0x{:02x}",
                description, static_cast<unsigned int>(node_id),
                static_cast<unsigned int>(expected_state));
            return true;
        }
    }

    /* A real transition still requires a fresh OnState publication newer than
     * the pre-command baseline. */
    std::unique_lock<std::mutex> lock(g_nmt_state_mutex);
    const bool reached = g_nmt_state_condition.wait_for(
        lock, timeout, [node_id, expected_state, baseline_generation]() {
            const NmtStateObservation& observation = g_nmt_states[node_id];
            return observation.generation > baseline_generation
                   && observation.state == expected_state;
        });
    if (reached) {
        return true;
    }

    const NmtStateObservation observation = g_nmt_states[node_id];
    if (observation.observed) {
        spdlog::error(
            "{} state confirmation timed out: node={} expected=0x{:02x} "
            "last=0x{:02x}",
            description, static_cast<unsigned int>(node_id),
            static_cast<unsigned int>(expected_state),
            static_cast<unsigned int>(observation.state));
    } else {
        spdlog::error(
            "{} state confirmation timed out: node={} expected=0x{:02x}; "
            "no remote NMT state event observed",
            description, static_cast<unsigned int>(node_id),
            static_cast<unsigned int>(expected_state));
    }
    return false;
}
