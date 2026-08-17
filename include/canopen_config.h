/**
 * @file
 * @brief CANopen host compile-time configuration shared across subsystems.
 *
 * @note Keep only settings whose semantics are shared by multiple CANopen
 *       modules or are intrinsic to the main runtime in this header. A setting
 *       remains module-owned when main.cpp only consumes it to register or
 *       enable that module. Protocol-specific and process-specific settings
 *       must be declared in the corresponding module header.
 */

#ifndef CANOPEN_CONFIG_H
#define CANOPEN_CONFIG_H

/** CANopen host role value for the existing AsyncMaster tester. */
#define CANOPEN_ROLE_MASTER 1
/** CANopen host role value for the Lely BasicSlave test peer. */
#define CANOPEN_ROLE_SLAVE 2
/** Active host role; change this macro when switching test roles. */
#define CANOPEN_ROLE CANOPEN_ROLE_MASTER

#if CANOPEN_ROLE != CANOPEN_ROLE_MASTER && CANOPEN_ROLE != CANOPEN_ROLE_SLAVE
#error "CANOPEN_ROLE must be CANOPEN_ROLE_MASTER or CANOPEN_ROLE_SLAVE"
#endif /* CANOPEN_ROLE != CANOPEN_ROLE_MASTER && CANOPEN_ROLE != CANOPEN_ROLE_SLAVE */

/** SocketCAN interface opened by the host application. */
#define CANOPEN_INTERFACE_NAME "can1"
/** Expected nominal CAN bitrate in bit/s. */
#define CANOPEN_EXPECTED_BITRATE 1000000

/** Local CANopen master node-ID used by the enabled A/B tester stages. */
#define CANOPEN_MASTER_NODE_ID 127
/** Managed MCU CANopen node-ID used by the enabled A/B tester stages. */
#define CANOPEN_SLAVE_NODE_ID 1
/** Lely software slave node-ID used by NMT-master validation. */
#define CANOPEN_PEER_NODE_ID 2
/** Producer heartbeat period of the software slave peer, in milliseconds. */
#define CANOPEN_PEER_HEARTBEAT_MS 500

/** Master DCF path relative to the deployed executable directory. */
#define CANOPEN_MASTER_DCF_PATH "../config/master.dcf"
/** EDS reused by the Lely software slave; node-ID is overridden at runtime. */
#define CANOPEN_PEER_EDS_PATH "../config/project.eds"

/** SocketCAN receive queue capacity. */
#define CANOPEN_CHANNEL_RX_QUEUE_SIZE 256
/** Maximum wait for Boot or NMT callback events. */
#define CANOPEN_WAIT_TIMEOUT_MS 5000
/** Maximum wait for the event-loop worker to publish startup. */
#define CANOPEN_LOOP_START_TIMEOUT_MS 1000

/** Asynchronous spdlog queue capacity. */
#define CANOPEN_LOG_QUEUE_SIZE 8192
/** Number of asynchronous spdlog worker threads. */
#define CANOPEN_LOG_WORKER_COUNT 1

#endif /* CANOPEN_CONFIG_H */
