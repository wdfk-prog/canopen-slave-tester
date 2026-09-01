/**
 * @file
 * @brief Implements ordered CANopen validation process execution.
 */

#include "canopen_process.h"

#include <spdlog/spdlog.h>

#include <cstddef>

int canopenRunProcesses(const CanopenProcessEntry* processes,
                        std::size_t process_count)
{
    /* An empty table is a valid build-time configuration when every optional
     * automatic validation process is disabled. */
    if (process_count == 0U) {
        spdlog::info("No automatic CANopen validation processes enabled");
        return 0;
    }

    /* A non-empty process count requires a concrete table so the runner never
     * dereferences an invalid registration array. */
    if (processes == nullptr) {
        spdlog::error("CANopen process table is null");
        return 1;
    }

    /* i selects the next process in the declared validation order. */
    for (std::size_t i = 0; i < process_count; ++i) {
        /* Keep a reference instead of copying the callable/name pair. */
        const CanopenProcessEntry& process = processes[i];
        if (process.name == nullptr || !process.handler) {
            spdlog::error("Invalid CANopen process entry at index {}", i);
            return 1;
        }

        spdlog::info("{} started", process.name);
        /* A zero handler result is the common process success contract. */
        const int result = process.handler();
        if (result != 0) {
            spdlog::error("{} failed with result={}", process.name, result);
            return 1;
        }
        spdlog::info("{} passed", process.name);
    }

    return 0;
}
