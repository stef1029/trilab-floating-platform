#ifndef ADELIE_PROTOCOL_H
#define ADELIE_PROTOCOL_H

#include <stdint.h>

/* Fits in one ATT write/notification at the default 23-byte ATT MTU. */
#define ADELIE_FRAME_MAGIC 0xAD1EU
#define ADELIE_FRAME_VERSION 1U
#define ADELIE_FRAME_SIZE 20U

/* Requests sent from Adelie to Korora. */
#define ADELIE_MSG_CLOCK_SYNC_REQUEST 0x01U
#define ADELIE_MSG_FAIRY_DO_NOW_REQUEST 0x02U

/* Clock reply: timestamp=t2, value=(t3-t2) in Korora ticks. */
#define ADELIE_MSG_CLOCK_REPLY 0x81U

/* Command-stage notifications sent from Korora to Adelie. */
#define ADELIE_MSG_COMMAND_KORORA_RX 0x90U
#define ADELIE_MSG_COMMAND_FAIRY_TX_START 0x91U
#define ADELIE_MSG_COMMAND_FAIRY_RX 0x92U
#define ADELIE_MSG_COMMAND_FAIRY_EXEC 0x93U
#define ADELIE_MSG_COMMAND_KORORA_ACK_RX 0x94U
#define ADELIE_MSG_COMMAND_KORORA_DONE_TX 0x95U
#define ADELIE_MSG_ERROR 0xFFU

#define ADELIE_STATUS_OK 0U
#define ADELIE_STATUS_BUSY 1U
#define ADELIE_STATUS_BAD_FRAME 2U
#define ADELIE_STATUS_I2C_WRITE_FAILED 3U
#define ADELIE_STATUS_TIMEOUT 4U
#define ADELIE_STATUS_FAIRY_MODEL_INVALID 5U
#define ADELIE_STATUS_TOKEN_MISMATCH 6U
#define ADELIE_STATUS_NOTIFY_FAILED 7U

/*
 * Compatibility aliases for older Adelie scripts. The wire values do not
 * change, so old and new software can interoperate during migration.
 */
#define ADELIE_MSG_REWARD_DO_NOW_REQUEST ADELIE_MSG_FAIRY_DO_NOW_REQUEST
#define ADELIE_MSG_COMMAND_I2C_TX_START ADELIE_MSG_COMMAND_FAIRY_TX_START
#define ADELIE_MSG_COMMAND_REWARD_RX ADELIE_MSG_COMMAND_FAIRY_RX
#define ADELIE_MSG_COMMAND_REWARD_EXEC ADELIE_MSG_COMMAND_FAIRY_EXEC
#define ADELIE_STATUS_REWARD_MODEL_INVALID ADELIE_STATUS_FAIRY_MODEL_INVALID

/*
 * Little-endian wire layout:
 *   0  u16 magic
 *   2  u8  version
 *   3  u8  message_type
 *   4  u32 sequence
 *   8  u64 timestamp
 *  16  u32 value/status/delta
 */
#define ADELIE_OFFSET_MAGIC 0U
#define ADELIE_OFFSET_VERSION 2U
#define ADELIE_OFFSET_TYPE 3U
#define ADELIE_OFFSET_SEQUENCE 4U
#define ADELIE_OFFSET_TIMESTAMP 8U
#define ADELIE_OFFSET_VALUE 16U

#endif
