/**
 * @file
 * @brief CANopen validation process registration and execution interface.
 */

#ifndef CANOPEN_PROCESS_H
#define CANOPEN_PROCESS_H

#include <cstddef>

namespace lely {
namespace canopen {
class AsyncMaster;
} // namespace canopen
} // namespace lely

/** CANopen validation process callback. */
using CanopenProcessHandler = int (*)(lely::canopen::AsyncMaster& master);

/** Registered CANopen validation process. */
struct CanopenProcessEntry {
    const char* name; /**< Human-readable process name used in logs. */
    CanopenProcessHandler handler; /**< Process implementation callback. */
};

/**
 * @brief Run registered CANopen validation processes in order.
 *
 * Execution stops at the first failed process.
 *
 * @param master Active Lely asynchronous CANopen master.
 * @param processes Process table; may be null only when process_count is zero.
 * @param process_count Number of entries in the process table.
 * @retval 0 All registered processes passed.
 * @retval 1 A process failed or the process table is invalid.
 */
int canopenRunProcesses(lely::canopen::AsyncMaster& master,
                        const CanopenProcessEntry* processes,
                        std::size_t process_count);

#endif /* CANOPEN_PROCESS_H */
