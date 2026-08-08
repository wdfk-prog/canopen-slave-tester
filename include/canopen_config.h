/**
 * @file
 * @brief CANopen master compile-time configuration.
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
/** Producer Heartbeat period used in both directions. */
#define CANOPEN_HEARTBEAT_PERIOD_MS 500
/** Consumer timeout multiplier relative to the producer period. */
#define CANOPEN_HEARTBEAT_MULTIPLIER 3

/** Enable the A01 bidirectional Heartbeat verification process. */
#define CANOPEN_ENABLE_HEARTBEAT_PROCESS 1
/** Enable the A02 user OD SDO verification process. */
#define CANOPEN_ENABLE_SDO_PROCESS 1
/** Enable Reset Communication during application shutdown. */
#define CANOPEN_ENABLE_FINAL_RESET_PROCESS 1

/** Asynchronous spdlog queue capacity. */
#define CANOPEN_LOG_QUEUE_SIZE 8192
/** Number of asynchronous spdlog worker threads. */
#define CANOPEN_LOG_WORKER_COUNT 1

#endif /* CANOPEN_CONFIG_H */
