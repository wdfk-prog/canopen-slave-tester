/**
 * @file
 * @brief Implements passive NMT transition callback tracing for the BasicSlave peer.
 */

#include "slave_peer_trace.h"

#include <lely/coapp/node.hpp>
#include <lely/coapp/slave.hpp>

#include <spdlog/spdlog.h>

#include <functional>

namespace {

const char* nmtCommandName(lely::canopen::NmtCommand command) noexcept
{
    switch (command) {
    case lely::canopen::NmtCommand::START:
        return "START";
    case lely::canopen::NmtCommand::STOP:
        return "STOP";
    case lely::canopen::NmtCommand::ENTER_PREOP:
        return "PREOP";
    case lely::canopen::NmtCommand::RESET_NODE:
        return "RESET_NODE";
    case lely::canopen::NmtCommand::RESET_COMM:
        return "RESET_COMM";
    }
    return "UNKNOWN";
}

void nmtTraceCallback(lely::canopen::NmtCommand command) noexcept
{
    spdlog::info("CANopen slave peer NMT transition callback: {}",
                 nmtCommandName(command));
}

} // namespace

void installSlavePeerNmtTrace(lely::canopen::BasicSlave& slave)
{
    slave.OnCommand(nmtTraceCallback);
}

void uninstallSlavePeerNmtTrace(lely::canopen::BasicSlave& slave)
{
    slave.OnCommand(std::function<void(lely::canopen::NmtCommand)>{});
}
