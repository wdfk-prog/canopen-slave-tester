/**
 * @file
 * @brief CANopen master compile-time configuration shared across subsystems.
 *
 * @note Keep only settings whose semantics are shared by multiple CANopen
 *       modules or are intrinsic to the main runtime in this header. A setting
 *       remains module-owned when main.cpp only consumes it to register or
 *       enable that module. Protocol-specific and process-specific settings
 *       must be declared in the corresponding module header.
 */

#ifndef CANOPEN_CONFIG_H
#define CANOPEN_CONFIG_H

/** SocketCAN interface opened by the master. */
#define CANOPEN_INTERFACE_NAME "can1"
/** Expected nominal CAN bitrate in bit/s. */
#define CANOPEN_EXPECTED_BITRATE 1000000

/** Local CANopen master node-ID. */
#define CANOPEN_MASTER_NODE_ID 127
/** Managed remote CANopen node-ID. */
#define CANOPEN_SLAVE_NODE_ID 1

/** Master DCF path relative to the deployed executable directory. */
#define CANOPEN_MASTER_DCF_PATH "../config/master.dcf"

/** SocketCAN receive queue capacity. */
#define CANOPEN_CHANNEL_RX_QUEUE_SIZE 256
/** Maximum wait for Boot or NMT callback events. */
#define CANOPEN_WAIT_TIMEOUT_MS 5000

/** Asynchronous spdlog queue capacity. */
#define CANOPEN_LOG_QUEUE_SIZE 8192
/** Number of asynchronous spdlog worker threads. */
#define CANOPEN_LOG_WORKER_COUNT 1

#endif /* CANOPEN_CONFIG_H */
