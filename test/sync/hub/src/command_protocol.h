#ifndef COMMAND_PROTOCOL_H
#define COMMAND_PROTOCOL_H

#include <stdint.h>

/* Fits in one ATT write/notification at the default 23-byte ATT MTU. */
#define HOST_FRAME_MAGIC 0xAD1EU
#define HOST_FRAME_VERSION 1U
#define HOST_FRAME_SIZE 20U

/* Requests sent from Host to the hub. */
#define HOST_MSG_CLOCK_SYNC_REQUEST 0x01U
#define HOST_MSG_NODE_DO_NOW_REQUEST 0x02U

/* Clock reply: timestamp=t2, value=(t3-t2) in the hub ticks. */
#define HOST_MSG_CLOCK_REPLY 0x81U

/* Command-stage notifications sent from the hub to Host. */
#define HOST_MSG_COMMAND_HUB_RX 0x90U
#define HOST_MSG_COMMAND_NODE_TX_START 0x91U
#define HOST_MSG_COMMAND_NODE_RX 0x92U
#define HOST_MSG_COMMAND_NODE_EXEC 0x93U
#define HOST_MSG_COMMAND_HUB_ACK_RX 0x94U
#define HOST_MSG_COMMAND_HUB_DONE_TX 0x95U
#define HOST_MSG_ERROR 0xFFU

#define HOST_STATUS_OK 0U
#define HOST_STATUS_BUSY 1U
#define HOST_STATUS_BAD_FRAME 2U
#define HOST_STATUS_I2C_WRITE_FAILED 3U
#define HOST_STATUS_TIMEOUT 4U
#define HOST_STATUS_NODE_MODEL_INVALID 5U
#define HOST_STATUS_TOKEN_MISMATCH 6U
#define HOST_STATUS_NOTIFY_FAILED 7U

/*
 * Compatibility aliases for older Host scripts. The wire values do not
 * change, so old and new software can interoperate during migration.
 */
#define HOST_MSG_REWARD_DO_NOW_REQUEST HOST_MSG_NODE_DO_NOW_REQUEST
#define HOST_MSG_COMMAND_I2C_TX_START HOST_MSG_COMMAND_NODE_TX_START
#define HOST_MSG_COMMAND_REWARD_RX HOST_MSG_COMMAND_NODE_RX
#define HOST_MSG_COMMAND_REWARD_EXEC HOST_MSG_COMMAND_NODE_EXEC
#define HOST_STATUS_REWARD_MODEL_INVALID HOST_STATUS_NODE_MODEL_INVALID

/*
 * Little-endian wire layout:
 *   0  u16 magic
 *   2  u8  version
 *   3  u8  message_type
 *   4  u32 sequence
 *   8  u64 timestamp
 *  16  u32 value/status/delta
 */
#define HOST_OFFSET_MAGIC 0U
#define HOST_OFFSET_VERSION 2U
#define HOST_OFFSET_TYPE 3U
#define HOST_OFFSET_SEQUENCE 4U
#define HOST_OFFSET_TIMESTAMP 8U
#define HOST_OFFSET_VALUE 16U

#endif
