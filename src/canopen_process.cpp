/**
 * @file
 * @brief Implements ordered CANopen validation process execution.
 */

#include "canopen_process.h"

#include <lely/coapp/master.hpp>

#include <spdlog/spdlog.h>

#include <cstddef>

int canopenRunProcesses(lely::canopen::AsyncMaster& master,
                        const CanopenProcessEntry* processes,
                        std::size_t process_count)
{
    if (process_count == 0U) {
        spdlog::info("No automatic CANopen validation processes enabled");
        return 0;
    }
    if (processes == nullptr) {
        spdlog::error("CANopen process table is null");
        return 1;
    }

    for (std::size_t i = 0; i < process_count; ++i) {
        const CanopenProcessEntry& process = processes[i];
        if (process.name == nullptr || process.handler == nullptr) {
            spdlog::error("Invalid CANopen process entry at index {}", i);
            return 1;
        }

        spdlog::info("{} started", process.name);
        const int result = process.handler(master);
        if (result != 0) {
            spdlog::error("{} failed with result={}", process.name, result);
            return 1;
        }
        spdlog::info("{} passed", process.name);
    }

    return 0;
}
