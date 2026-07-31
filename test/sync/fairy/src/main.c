/*
 * fairy - NUCLEO-G071RB timing and target controller
 *
 * Build: PlatformIO + STM32Cube HAL
 * Board: nucleo_g071rb
 *
 * RS-485 connections:
 *   PC4 / D1    -> D   (USART1_TX)
 *   PC5 / D0    <- R   (USART1_RX)
 *   PB5 / D4    -> DE and /RE tied together
 *   A/B/GND    <-> Korora RS-485 transceiver A/B/GND
 *
 * Timing connections retained:
 *   PA0 / A0   <- Korora SYNC pulse (TIM2_CH1)
 *   PA1 / A1   <- shared reward/event input (TIM2_CH2)
 *   PA2/PA3    <-> ST-LINK virtual COM port (USART2 debug)
 *
 * The RS-485 protocol is addressed and retry-safe. A POLL response containing
 * a record is removed from the FIFO only when the following POLL acknowledges
 * that response sequence. Retrying the same request therefore cannot silently
 * lose or duplicate a record.
 */

#include "stm32g0xx_hal.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define REWARD_FRAME_MAGIC 0xA5U
#define REWARD_FRAME_VERSION 2U
#define REWARD_FRAME_SIZE 40U

#define FRAME_OFFSET_MAGIC 0U
#define FRAME_OFFSET_VERSION 1U
#define FRAME_OFFSET_STATUS_FLAGS 2U
#define FRAME_OFFSET_LENGTH 3U
#define FRAME_OFFSET_RECORD_TYPE 4U
#define FRAME_OFFSET_PENDING_COUNT 5U
#define FRAME_OFFSET_RECORD_FLAGS 6U
#define FRAME_OFFSET_CAPTURE_TICKS 8U
#define FRAME_OFFSET_SNAPSHOT_TICKS 16U
#define FRAME_OFFSET_CAPTURE_LOSS_COUNT 24U
#define FRAME_OFFSET_TRANSPORT_ERROR_COUNT 28U
#define FRAME_OFFSET_AUXILIARY 32U
#define FRAME_OFFSET_CRC 36U

#define REWARD_STATUS_RECORD_VALID (1U << 0)
#define REWARD_STATUS_FIRST_AFTER_RESET (1U << 1)
#define REWARD_STATUS_CAPTURE_LOSS_LATCHED (1U << 2)
#define REWARD_STATUS_TRANSPORT_ERROR_LATCHED (1U << 3)
#define REWARD_STATUS_CLOCK_FAULT (1U << 4)

#define REWARD_RECORD_NONE 0U
#define REWARD_RECORD_SYNC 1U
#define REWARD_RECORD_EVENT 2U
#define REWARD_RECORD_COMMAND_ACK 3U

#define REWARD_RECORD_FLAG_HW_OVERCAPTURE (1U << 0)

#define REWARD_COMMAND_ACK_RESET_SEEN 0x01U
#define REWARD_COMMAND_CLEAR_LATCHED_FLAGS 0x02U
#define REWARD_COMMAND_DO_NOW_MASK 0x80U
#define REWARD_COMMAND_TOKEN_MASK 0x7FU

#define FAIRY_RS485_ADDRESS 1U
#define FAIRY_RS485_BAUD_RATE 460800
#define FAIRY_RS485_REQUEST_MAGIC_0 0xA6U
#define FAIRY_RS485_REQUEST_MAGIC_1 0x5AU
#define FAIRY_RS485_RESPONSE_MAGIC_0 0xA6U
#define FAIRY_RS485_RESPONSE_MAGIC_1 0xA5U
#define FAIRY_RS485_OPCODE_POLL 0x10U
#define FAIRY_RS485_OPCODE_COMMAND 0x11U
#define FAIRY_RS485_RESPONSE_BIT 0x80U
#define FAIRY_RS485_NO_ACK 0xFFU
#define FAIRY_RS485_REQUEST_SIZE 8U
#define FAIRY_RS485_COMMAND_RESPONSE_SIZE 8U
#define FAIRY_RS485_POLL_RESPONSE_SIZE (6U + REWARD_FRAME_SIZE + 2U)
#define FAIRY_RS485_MAX_RESPONSE_SIZE FAIRY_RS485_POLL_RESPONSE_SIZE
#define FAIRY_RS485_STATUS_OK 0U
#define FAIRY_RS485_STATUS_BAD_COMMAND 1U
#define FAIRY_RS485_STATUS_QUEUE_FULL 2U
#define FAIRY_RS485_TX_TIMEOUT_MS 10U
#define FAIRY_RS485_TURNAROUND_TICKS 640U /* 40 us at TIM2 = 16 MHz */

#define CAPTURE_QUEUE_CAPACITY 16U

/* Must be a power of two. */
#define DEBUG_UART_TX_CAPACITY 512U
#define DEBUG_UART_TX_MASK (DEBUG_UART_TX_CAPACITY - 1U)
#define DEBUG_HEARTBEAT_MS 1000U

struct capture_record {
  uint64_t ticks;
  uint64_t secondary_ticks;
  uint32_t auxiliary;
  uint8_t type;
  uint8_t flags;
};

TIM_HandleTypeDef htim2;
UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;

static volatile uint32_t timer_overflow_high;
static volatile uint32_t capture_loss_count;
static volatile uint32_t transport_error_count;
static volatile uint8_t latched_status_flags;
static uint32_t reset_cause;

static volatile struct capture_record capture_queue[CAPTURE_QUEUE_CAPACITY];
static volatile uint8_t capture_queue_head;
static volatile uint8_t capture_queue_tail;
static volatile uint8_t capture_queue_count;

static uint8_t rs485_rx_block[FAIRY_RS485_REQUEST_SIZE];
static volatile uint8_t rs485_request_buffer[FAIRY_RS485_REQUEST_SIZE];
static volatile uint8_t rs485_request_index;
static volatile bool rs485_response_pending;
static volatile uint8_t rs485_response_length;
static uint8_t rs485_response_buffer[FAIRY_RS485_MAX_RESPONSE_SIZE];

static bool rs485_last_request_valid;
static uint8_t rs485_last_request_opcode;
static uint8_t rs485_last_request_sequence;
static uint8_t rs485_last_request_value;
static uint8_t rs485_last_response[FAIRY_RS485_MAX_RESPONSE_SIZE];
static uint8_t rs485_last_response_length;

static bool rs485_outstanding_record;
static uint8_t rs485_outstanding_sequence;
static uint64_t rs485_outstanding_record_ticks;
static uint8_t rs485_outstanding_record_type;

static volatile uint32_t rs485_request_count;
static volatile uint32_t rs485_duplicate_request_count;
static volatile uint32_t rs485_response_sent_count;
static volatile uint32_t rs485_response_tx_error_count;
static volatile uint32_t rs485_response_overwrite_count;

static volatile uint32_t do_now_counter;

static volatile bool command_debug_pending;
static volatile uint8_t command_debug_token;
static volatile bool command_debug_queued;
static volatile uint64_t command_debug_received_ticks;
static volatile uint64_t command_debug_completed_ticks;

static volatile uint8_t debug_uart_tx_buffer[DEBUG_UART_TX_CAPACITY];
static volatile uint16_t debug_uart_tx_head;
static volatile uint16_t debug_uart_tx_tail;
static volatile uint32_t debug_uart_dropped_bytes;

static volatile uint32_t rs485_rx_byte_count;
static volatile uint32_t rs485_framing_error_count;
static volatile uint32_t rs485_noise_error_count;
static volatile uint32_t rs485_overrun_error_count;
static volatile uint32_t rs485_parity_error_count;

static void SystemClock_Config(void);
static void GPIO_Init(void);
static void USART1_Init(void);
static void USART2_Init(void);
static void TIM2_Init(void);
static void Error_Handler(void);
static void prepare_remote_snapshot(uint8_t frame[REWARD_FRAME_SIZE],
                                    bool *has_record_out,
                                    uint64_t *record_ticks_out,
                                    uint8_t *record_type_out);
static void service_rs485_response(void);
static void service_command_debug(void);
static void service_uart_heartbeat(void);

/*
 * Replace this with the real immediate action.
 * It runs when a complete RS-485 command request is received, so it must be
 * short, non-blocking, and ISR-safe.
 */
__attribute__((weak)) void reward_do_something_now_isr(void) {
  do_now_counter++;
  __DMB();
}

static uint16_t crc16_ccitt(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFFU;

  for (size_t i = 0U; i < length; ++i) {
    crc ^= (uint16_t)data[i] << 8;

    for (uint32_t bit = 0U; bit < 8U; ++bit) {
      if ((crc & 0x8000U) != 0U) {
        crc = (uint16_t)(((uint32_t)crc << 1) ^ 0x1021U);
      } else {
        crc = (uint16_t)((uint32_t)crc << 1);
      }
    }
  }

  return crc;
}

static void put_u16_le(uint8_t *destination, uint16_t value) {
  destination[0] = (uint8_t)(value & 0xFFU);
  destination[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void put_u32_le(uint8_t *destination, uint32_t value) {
  destination[0] = (uint8_t)(value & 0xFFU);
  destination[1] = (uint8_t)((value >> 8) & 0xFFU);
  destination[2] = (uint8_t)((value >> 16) & 0xFFU);
  destination[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static void put_u64_le(uint8_t *destination, uint64_t value) {
  put_u32_le(destination, (uint32_t)(value & 0xFFFFFFFFULL));
  put_u32_le(destination + 4U, (uint32_t)(value >> 32));
}

/* ---------------------------- Debug UART --------------------------------- */

static uint16_t debug_uart_next_index(uint16_t index) {
  return (uint16_t)((index + 1U) & DEBUG_UART_TX_MASK);
}

static void debug_uart_write(const char *text, size_t length) {
  if ((text == NULL) || (length == 0U)) {
    return;
  }

  const uint32_t saved_primask = __get_PRIMASK();
  __disable_irq();

  for (size_t i = 0U; i < length; ++i) {
    const uint16_t next = debug_uart_next_index(debug_uart_tx_head);

    if (next == debug_uart_tx_tail) {
      debug_uart_dropped_bytes += (uint32_t)(length - i);
      break;
    }

    debug_uart_tx_buffer[debug_uart_tx_head] = (uint8_t)text[i];
    debug_uart_tx_head = next;
  }

  SET_BIT(USART2->CR1, USART_CR1_TXEIE_TXFNFIE);

  if (saved_primask == 0U) {
    __enable_irq();
  }
}

static void debug_uart_puts(const char *text) {
  if (text != NULL) {
    debug_uart_write(text, strlen(text));
  }
}

static void debug_uart_logf(const char *format, ...) {
  char line[160];

  va_list arguments;
  va_start(arguments, format);
  const int length = vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);

  if (length <= 0) {
    return;
  }

  const size_t used =
      ((size_t)length < sizeof(line)) ? (size_t)length : (sizeof(line) - 1U);

  debug_uart_write(line, used);
}

void USART2_IRQHandler(void) {
  const uint32_t status = USART2->ISR;

  if (((status & USART_ISR_TXE_TXFNF) != 0U) &&
      ((USART2->CR1 & USART_CR1_TXEIE_TXFNFIE) != 0U)) {
    if (debug_uart_tx_tail != debug_uart_tx_head) {
      USART2->TDR = debug_uart_tx_buffer[debug_uart_tx_tail];
      debug_uart_tx_tail = debug_uart_next_index(debug_uart_tx_tail);
    } else {
      CLEAR_BIT(USART2->CR1, USART_CR1_TXEIE_TXFNFIE);
    }
  }
}

static void service_uart_heartbeat(void) {
  static uint32_t last_ms;
  static uint32_t heartbeat_index;

  const uint32_t now_ms = HAL_GetTick();

  if ((uint32_t)(now_ms - last_ms) < DEBUG_HEARTBEAT_MS) {
    return;
  }

  last_ms = now_ms;
  heartbeat_index++;

  debug_uart_logf("UART_ALIVE,%lu,RX,%lu,REQUESTS,%lu,DUPLICATES,%lu,"
                  "RESPONSES,%lu,TX_ERRORS,%lu,OVERWRITES,%lu,"
                  "FE,%lu,NE,%lu,ORE,%lu,PE,%lu\r\n",
                  (unsigned long)heartbeat_index,
                  (unsigned long)rs485_rx_byte_count,
                  (unsigned long)rs485_request_count,
                  (unsigned long)rs485_duplicate_request_count,
                  (unsigned long)rs485_response_sent_count,
                  (unsigned long)rs485_response_tx_error_count,
                  (unsigned long)rs485_response_overwrite_count,
                  (unsigned long)rs485_framing_error_count,
                  (unsigned long)rs485_noise_error_count,
                  (unsigned long)rs485_overrun_error_count,
                  (unsigned long)rs485_parity_error_count);
}

/* ---------------------------- Timer/FIFO --------------------------------- */

static uint64_t extend_capture_value(uint32_t captured_low,
                                     uint32_t timer_status) {
  uint32_t captured_high = timer_overflow_high;

  if (((timer_status & TIM_SR_UIF) != 0U) && (captured_low < 0x80000000UL)) {
    captured_high++;
  }

  return ((uint64_t)captured_high << 32) | captured_low;
}

static void queue_capture_locked(uint8_t type, uint64_t ticks,
                                 uint8_t record_flags) {
  if (capture_queue_count >= CAPTURE_QUEUE_CAPACITY) {
    capture_loss_count++;
    latched_status_flags |=
        REWARD_STATUS_CAPTURE_LOSS_LATCHED | REWARD_STATUS_CLOCK_FAULT;
    return;
  }

  const uint8_t tail = capture_queue_tail;

  capture_queue[tail].ticks = ticks;
  capture_queue[tail].secondary_ticks = 0ULL;
  capture_queue[tail].auxiliary = 0U;
  capture_queue[tail].type = type;
  capture_queue[tail].flags = record_flags;

  __DMB();

  capture_queue_tail = (uint8_t)((tail + 1U) % CAPTURE_QUEUE_CAPACITY);
  capture_queue_count++;
}

static bool queue_command_ack_locked(uint8_t token, uint64_t received_ticks,
                                     uint64_t completed_ticks) {
  if (capture_queue_count >= CAPTURE_QUEUE_CAPACITY) {
    capture_loss_count++;
    latched_status_flags |= REWARD_STATUS_CAPTURE_LOSS_LATCHED;
    return false;
  }

  const uint8_t tail = capture_queue_tail;

  capture_queue[tail].ticks = received_ticks;
  capture_queue[tail].secondary_ticks = completed_ticks;
  capture_queue[tail].auxiliary = token;
  capture_queue[tail].type = REWARD_RECORD_COMMAND_ACK;
  capture_queue[tail].flags = 0U;

  __DMB();

  capture_queue_tail = (uint8_t)((tail + 1U) % CAPTURE_QUEUE_CAPACITY);
  capture_queue_count++;

  return true;
}

static bool peek_capture_locked(struct capture_record *record,
                                uint8_t *pending_count) {
  *pending_count = capture_queue_count;

  if (capture_queue_count == 0U) {
    record->ticks = 0ULL;
    record->secondary_ticks = 0ULL;
    record->auxiliary = 0U;
    record->type = REWARD_RECORD_NONE;
    record->flags = 0U;
    return false;
  }

  const uint8_t head = capture_queue_head;

  record->ticks = capture_queue[head].ticks;
  record->secondary_ticks = capture_queue[head].secondary_ticks;
  record->auxiliary = capture_queue[head].auxiliary;
  record->type = capture_queue[head].type;
  record->flags = capture_queue[head].flags;

  return true;
}

static bool pop_expected_capture_locked(uint8_t expected_type,
                                        uint64_t expected_ticks) {
  if (capture_queue_count == 0U) {
    return false;
  }

  const uint8_t head = capture_queue_head;

  if ((capture_queue[head].type != expected_type) ||
      (capture_queue[head].ticks != expected_ticks)) {
    return false;
  }

  capture_queue_head = (uint8_t)((head + 1U) % CAPTURE_QUEUE_CAPACITY);
  capture_queue_count--;
  return true;
}

static uint64_t timer_snapshot_locked(void) {
  uint32_t high = timer_overflow_high;
  const uint32_t low = TIM2->CNT;
  const uint32_t status = TIM2->SR;

  if (((status & TIM_SR_UIF) != 0U) && (low < 0x80000000UL)) {
    high++;
  }

  return ((uint64_t)high << 32) | low;
}

static void timer_busy_wait_ticks(uint32_t ticks) {
  const uint32_t start = TIM2->CNT;
  while ((uint32_t)(TIM2->CNT - start) < ticks) {
  }
}

/* -------------------------- Command handling ------------------------------ */

static uint8_t process_rs485_command_from_isr(uint8_t command,
                                              uint64_t received_ticks) {
  if (command == REWARD_COMMAND_ACK_RESET_SEEN) {
    latched_status_flags &= (uint8_t)~REWARD_STATUS_FIRST_AFTER_RESET;
    return FAIRY_RS485_STATUS_OK;
  }

  if (command == REWARD_COMMAND_CLEAR_LATCHED_FLAGS) {
    latched_status_flags &= (uint8_t)~(REWARD_STATUS_CAPTURE_LOSS_LATCHED |
                                       REWARD_STATUS_TRANSPORT_ERROR_LATCHED |
                                       REWARD_STATUS_CLOCK_FAULT);
    return FAIRY_RS485_STATUS_OK;
  }

  if ((command & REWARD_COMMAND_DO_NOW_MASK) == 0U) {
    return FAIRY_RS485_STATUS_BAD_COMMAND;
  }

  const uint8_t token = command & REWARD_COMMAND_TOKEN_MASK;
  reward_do_something_now_isr();

  const uint32_t saved_primask = __get_PRIMASK();
  __disable_irq();
  const uint64_t completed_ticks = timer_snapshot_locked();
  const bool queued =
      queue_command_ack_locked(token, received_ticks, completed_ticks);

  command_debug_token = token;
  command_debug_received_ticks = received_ticks;
  command_debug_completed_ticks = completed_ticks;
  command_debug_queued = queued;
  __DMB();
  command_debug_pending = true;
  if (saved_primask == 0U) {
    __enable_irq();
  }

  return queued ? FAIRY_RS485_STATUS_OK : FAIRY_RS485_STATUS_QUEUE_FULL;
}

static void service_command_debug(void) {
  uint8_t token;
  bool queued;
  uint64_t received_ticks;
  uint64_t completed_ticks;
  uint32_t request_count;
  uint32_t duplicate_count;

  const uint32_t saved_primask = __get_PRIMASK();
  __disable_irq();

  if (!command_debug_pending) {
    if (saved_primask == 0U) {
      __enable_irq();
    }
    return;
  }

  token = command_debug_token;
  queued = command_debug_queued;
  received_ticks = command_debug_received_ticks;
  completed_ticks = command_debug_completed_ticks;
  request_count = rs485_request_count;
  duplicate_count = rs485_duplicate_request_count;
  command_debug_pending = false;

  if (saved_primask == 0U) {
    __enable_irq();
  }

  debug_uart_logf(
      "REWARD_COMMAND,%u,%u,%llu,%llu,RS485_REQUESTS,%lu,DUPLICATES,%lu\r\n",
      token, queued ? 1U : 0U, (unsigned long long)received_ticks,
      (unsigned long long)completed_ticks, (unsigned long)request_count,
      (unsigned long)duplicate_count);
}

/* ---------------------- Shared remote frame ----------------------------- */

static void prepare_remote_snapshot(uint8_t frame[REWARD_FRAME_SIZE],
                                    bool *has_record_out,
                                    uint64_t *record_ticks_out,
                                    uint8_t *record_type_out) {
  struct capture_record record;
  uint8_t pending_count;
  uint8_t status_flags;
  uint64_t snapshot_ticks;
  uint32_t loss_count;
  uint32_t error_count;

  const uint32_t saved_primask = __get_PRIMASK();
  __disable_irq();

  const bool has_record = peek_capture_locked(&record, &pending_count);
  status_flags = latched_status_flags;
  if (has_record) {
    status_flags |= REWARD_STATUS_RECORD_VALID;
  }

  snapshot_ticks = (record.type == REWARD_RECORD_COMMAND_ACK)
                       ? record.secondary_ticks
                       : timer_snapshot_locked();

  loss_count = capture_loss_count;
  error_count = transport_error_count;

  if (saved_primask == 0U) {
    __enable_irq();
  }

  memset(frame, 0, REWARD_FRAME_SIZE);
  frame[FRAME_OFFSET_MAGIC] = REWARD_FRAME_MAGIC;
  frame[FRAME_OFFSET_VERSION] = REWARD_FRAME_VERSION;
  frame[FRAME_OFFSET_STATUS_FLAGS] = status_flags;
  frame[FRAME_OFFSET_LENGTH] = REWARD_FRAME_SIZE;
  frame[FRAME_OFFSET_RECORD_TYPE] = record.type;
  frame[FRAME_OFFSET_PENDING_COUNT] = pending_count;
  frame[FRAME_OFFSET_RECORD_FLAGS] = record.flags;

  put_u64_le(&frame[FRAME_OFFSET_CAPTURE_TICKS], record.ticks);
  put_u64_le(&frame[FRAME_OFFSET_SNAPSHOT_TICKS], snapshot_ticks);
  put_u32_le(&frame[FRAME_OFFSET_CAPTURE_LOSS_COUNT], loss_count);
  put_u32_le(&frame[FRAME_OFFSET_TRANSPORT_ERROR_COUNT], error_count);
  put_u32_le(&frame[FRAME_OFFSET_AUXILIARY],
             (record.type == REWARD_RECORD_COMMAND_ACK) ? record.auxiliary
                                                        : reset_cause);

  put_u16_le(&frame[FRAME_OFFSET_CRC], crc16_ccitt(frame, FRAME_OFFSET_CRC));

  *has_record_out = has_record;
  *record_ticks_out = record.ticks;
  *record_type_out = record.type;
}

static void rs485_encode_response(uint8_t *response, uint8_t opcode,
                                  uint8_t sequence, uint8_t field,
                                  size_t response_length) {
  response[0] = FAIRY_RS485_RESPONSE_MAGIC_0;
  response[1] = FAIRY_RS485_RESPONSE_MAGIC_1;
  response[2] = FAIRY_RS485_ADDRESS;
  response[3] = (uint8_t)(opcode | FAIRY_RS485_RESPONSE_BIT);
  response[4] = sequence;
  response[5] = field;
  put_u16_le(&response[response_length - 2U],
             crc16_ccitt(response, response_length - 2U));
}

static void rs485_publish_response_from_isr(const uint8_t *response,
                                            uint8_t response_length) {
  if (rs485_response_pending) {
    rs485_response_overwrite_count++;
  }

  memcpy(rs485_response_buffer, response, response_length);
  rs485_response_length = response_length;
  __DMB();
  rs485_response_pending = true;
}

static void rs485_cache_and_publish_from_isr(uint8_t opcode, uint8_t sequence,
                                             uint8_t value,
                                             const uint8_t *response,
                                             uint8_t response_length) {
  rs485_last_request_valid = true;
  rs485_last_request_opcode = opcode;
  rs485_last_request_sequence = sequence;
  rs485_last_request_value = value;
  rs485_last_response_length = response_length;
  memcpy(rs485_last_response, response, response_length);
  rs485_publish_response_from_isr(response, response_length);
}

static void rs485_acknowledge_record_from_isr(uint8_t ack_sequence) {
  if (!rs485_outstanding_record ||
      (ack_sequence != rs485_outstanding_sequence)) {
    return;
  }

  const bool popped = pop_expected_capture_locked(
      rs485_outstanding_record_type, rs485_outstanding_record_ticks);
  rs485_outstanding_record = false;

  if (!popped) {
    latched_status_flags |= REWARD_STATUS_CLOCK_FAULT;
  }
}

static void rs485_handle_request_from_isr(void) {
  uint8_t request[FAIRY_RS485_REQUEST_SIZE];
  for (size_t i = 0U; i < FAIRY_RS485_REQUEST_SIZE; ++i) {
    request[i] = rs485_request_buffer[i];
  }

  if ((request[0] != FAIRY_RS485_REQUEST_MAGIC_0) ||
      (request[1] != FAIRY_RS485_REQUEST_MAGIC_1)) {
    return;
  }

  if (request[2] != FAIRY_RS485_ADDRESS) {
    return;
  }

  const uint16_t received_crc =
      (uint16_t)request[6] | ((uint16_t)request[7] << 8);
  if (received_crc != crc16_ccitt(request, FAIRY_RS485_REQUEST_SIZE - 2U)) {
    transport_error_count++;
    latched_status_flags |= REWARD_STATUS_TRANSPORT_ERROR_LATCHED;
    return;
  }

  const uint8_t opcode = request[3];
  const uint8_t sequence = request[4];
  const uint8_t value = request[5];
  rs485_request_count++;

  if (rs485_last_request_valid && (opcode == rs485_last_request_opcode) &&
      (sequence == rs485_last_request_sequence) &&
      (value == rs485_last_request_value)) {
    rs485_duplicate_request_count++;
    rs485_publish_response_from_isr(rs485_last_response,
                                    rs485_last_response_length);
    return;
  }

  if (opcode == FAIRY_RS485_OPCODE_POLL) {
    rs485_acknowledge_record_from_isr(value);

    bool has_record;
    uint64_t record_ticks;
    uint8_t record_type;
    uint8_t response[FAIRY_RS485_POLL_RESPONSE_SIZE] = {0};

    prepare_remote_snapshot(&response[6], &has_record, &record_ticks,
                            &record_type);
    rs485_encode_response(response, opcode, sequence, REWARD_FRAME_SIZE,
                          sizeof(response));

    rs485_outstanding_record = has_record;
    if (has_record) {
      rs485_outstanding_sequence = sequence;
      rs485_outstanding_record_ticks = record_ticks;
      rs485_outstanding_record_type = record_type;
    }

    rs485_cache_and_publish_from_isr(opcode, sequence, value, response,
                                     sizeof(response));
    return;
  }

  if (opcode == FAIRY_RS485_OPCODE_COMMAND) {
    const uint32_t saved_primask = __get_PRIMASK();
    __disable_irq();
    const uint64_t received_ticks = timer_snapshot_locked();
    if (saved_primask == 0U) {
      __enable_irq();
    }

    const uint8_t status =
        process_rs485_command_from_isr(value, received_ticks);
    uint8_t response[FAIRY_RS485_COMMAND_RESPONSE_SIZE] = {0};
    rs485_encode_response(response, opcode, sequence, status, sizeof(response));
    rs485_cache_and_publish_from_isr(opcode, sequence, value, response,
                                     sizeof(response));
  }
}

static void rs485_consume_byte_from_isr(uint8_t byte) {
  if (rs485_request_index == 0U) {
    if (byte == FAIRY_RS485_REQUEST_MAGIC_0) {
      rs485_request_buffer[0] = byte;
      rs485_request_index = 1U;
    }
    return;
  }

  if (rs485_request_index == 1U) {
    if (byte == FAIRY_RS485_REQUEST_MAGIC_1) {
      rs485_request_buffer[1] = byte;
      rs485_request_index = 2U;
    } else if (byte == FAIRY_RS485_REQUEST_MAGIC_0) {
      rs485_request_buffer[0] = byte;
      rs485_request_index = 1U;
    } else {
      rs485_request_index = 0U;
    }
    return;
  }

  rs485_request_buffer[rs485_request_index++] = byte;
  if (rs485_request_index >= FAIRY_RS485_REQUEST_SIZE) {
    rs485_request_index = 0U;
    rs485_handle_request_from_isr();
  }
}

static void service_rs485_response(void) {
  uint8_t response[FAIRY_RS485_MAX_RESPONSE_SIZE];
  uint8_t response_length;

  const uint32_t saved_primask = __get_PRIMASK();
  __disable_irq();
  if (!rs485_response_pending) {
    if (saved_primask == 0U) {
      __enable_irq();
    }
    return;
  }

  response_length = rs485_response_length;
  memcpy(response, rs485_response_buffer, response_length);
  rs485_response_pending = false;
  if (saved_primask == 0U) {
    __enable_irq();
  }

  /* Give Korora time to release DE and enable its receiver. */
  timer_busy_wait_ticks(FAIRY_RS485_TURNAROUND_TICKS);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_SET);
  for (volatile uint32_t delay = 0U; delay < 16U; ++delay) {
    __NOP();
  }

  const HAL_StatusTypeDef status = HAL_UART_Transmit(
      &huart1, response, response_length, FAIRY_RS485_TX_TIMEOUT_MS);

  if (status == HAL_OK) {
    rs485_response_sent_count++;
  } else {
    rs485_response_tx_error_count++;
  }
  while ((status == HAL_OK) &&
         (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_TC) == RESET)) {
  }
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_RESET);

  if (status != HAL_OK) {
    const uint32_t error_primask = __get_PRIMASK();
    __disable_irq();
    transport_error_count++;
    latched_status_flags |= REWARD_STATUS_TRANSPORT_ERROR_LATCHED;
    if (error_primask == 0U) {
      __enable_irq();
    }
  }
}

/* ------------------------------ Main ------------------------------------- */

static void RS485_Static_Low_Test(void) {
  GPIO_InitTypeDef gpio = {0};

  /*
   * Set the output value before changing PC4 to GPIO output,
   * avoiding a brief high pulse.
   */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_RESET);

  gpio.Pin = GPIO_PIN_4;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &gpio);
}

int main(void) {
  reset_cause = RCC->CSR;

  HAL_Init();
  __HAL_RCC_CLEAR_RESET_FLAGS();

  latched_status_flags = REWARD_STATUS_FIRST_AFTER_RESET;

  SystemClock_Config();
  GPIO_Init();
  USART2_Init();
  TIM2_Init();
  USART1_Init();

  debug_uart_puts("\r\n"
                  "FAIRY_BOOT,G071RB\r\n"
                  "FAIRY_RS485,address=1,baud=460800,protocol=1\r\n");

  if (HAL_UART_Receive_IT(&huart1, rs485_rx_block, sizeof(rs485_rx_block)) !=
      HAL_OK) {
    Error_Handler();
  }

  while (1) {
    service_rs485_response();
    service_command_debug();
    service_uart_heartbeat();
    __WFI();
  }
}

/* -------------------------- Initialization ------------------------------- */

static void SystemClock_Config(void) {
  RCC_OscInitTypeDef oscillator = {0};
  RCC_ClkInitTypeDef clock = {0};

  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

  oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  oscillator.HSIState = RCC_HSI_ON;
  oscillator.HSIDiv = RCC_HSI_DIV1;
  oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  oscillator.PLL.PLLState = RCC_PLL_NONE;

  if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) {
    Error_Handler();
  }

  clock.ClockType =
      RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1;
  clock.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clock.APB1CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_0) != HAL_OK) {
    Error_Handler();
  }
}

static void GPIO_Init(void) {
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  /*
   * PA0 / TIM2_CH1:
   * RS-485 receiver's normal logic output.
   * Idle is high and the transported sync pulse is active-low.
   */
  gpio.Pin = GPIO_PIN_0;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF2_TIM2;
  HAL_GPIO_Init(GPIOA, &gpio);

  /*
   * PA1 / TIM2_CH2:
   * Existing local reward/event input, active-high.
   */
  gpio.Pin = GPIO_PIN_1;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF2_TIM2;
  HAL_GPIO_Init(GPIOA, &gpio);

  gpio.Pin = GPIO_PIN_2 | GPIO_PIN_3;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF1_USART2;
  HAL_GPIO_Init(GPIOA, &gpio);

  /* Arduino D1: PC4 = USART1_TX. */
  gpio.Pin = GPIO_PIN_4;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF1_USART1;
  HAL_GPIO_Init(GPIOC, &gpio);

  /*
   * Arduino D0: PC5 = USART1_RX.
   * Pull up because the THVD1410 R output is high-Z while /RE is high.
   */
  gpio.Pin = GPIO_PIN_5;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF1_USART1;
  HAL_GPIO_Init(GPIOC, &gpio);

  /* PB5 drives tied THVD1410 DE and /RE. Default to receive. */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_RESET);
  gpio.Pin = GPIO_PIN_5;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &gpio);
}

static void USART1_Init(void) {
  RCC_PeriphCLKInitTypeDef peripheral_clock = {0};

  peripheral_clock.PeriphClockSelection = RCC_PERIPHCLK_USART1;
  peripheral_clock.Usart1ClockSelection = RCC_USART1CLKSOURCE_PCLK1;
  if (HAL_RCCEx_PeriphCLKConfig(&peripheral_clock) != HAL_OK) {
    Error_Handler();
  }

  __HAL_RCC_USART1_CLK_ENABLE();

  huart1.Instance = USART1;
  huart1.Init.BaudRate = FAIRY_RS485_BAUD_RATE;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

  if (HAL_UART_Init(&huart1) != HAL_OK) {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart1, UART_TXFIFO_THRESHOLD_1_8) !=
      HAL_OK) {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart1, UART_RXFIFO_THRESHOLD_1_8) !=
      HAL_OK) {
    Error_Handler();
  }
  if (HAL_UARTEx_EnableFifoMode(&huart1) != HAL_OK) {
    Error_Handler();
  }

  HAL_NVIC_SetPriority(USART1_IRQn, 1U, 0U);
  HAL_NVIC_EnableIRQ(USART1_IRQn);
}

static void USART2_Init(void) {
  RCC_PeriphCLKInitTypeDef peripheral_clock = {0};

  peripheral_clock.PeriphClockSelection = RCC_PERIPHCLK_USART2;
  peripheral_clock.Usart2ClockSelection = RCC_USART2CLKSOURCE_PCLK1;

  if (HAL_RCCEx_PeriphCLKConfig(&peripheral_clock) != HAL_OK) {
    Error_Handler();
  }

  __HAL_RCC_USART2_CLK_ENABLE();

  huart2.Instance = USART2;
  huart2.Init.BaudRate = 460800U;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

  if (HAL_UART_Init(&huart2) != HAL_OK) {
    Error_Handler();
  }

  if (HAL_UARTEx_SetTxFifoThreshold(&huart2, UART_TXFIFO_THRESHOLD_1_8) !=
      HAL_OK) {
    Error_Handler();
  }

  if (HAL_UARTEx_SetRxFifoThreshold(&huart2, UART_RXFIFO_THRESHOLD_1_8) !=
      HAL_OK) {
    Error_Handler();
  }

  if (HAL_UARTEx_DisableFifoMode(&huart2) != HAL_OK) {
    Error_Handler();
  }

  HAL_NVIC_SetPriority(USART2_IRQn, 2U, 0U);
  HAL_NVIC_EnableIRQ(USART2_IRQn);
}

static void TIM2_Init(void) {
  TIM_IC_InitTypeDef capture = {0};

  __HAL_RCC_TIM2_CLK_ENABLE();

  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0U;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 0xFFFFFFFFUL;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

  if (HAL_TIM_IC_Init(&htim2) != HAL_OK) {
    Error_Handler();
  }

  capture.ICSelection = TIM_ICSELECTION_DIRECTTI;
  capture.ICPrescaler = TIM_ICPSC_DIV1;

  /*
   * Start with no digital filter while testing the RS-485 pulse path.
   * A filter can be added later if real cable noise causes false captures.
   */
  capture.ICFilter = 0U;

  /* Sync input: idle-high, active-low, timestamp the falling edge. */
  capture.ICPolarity = TIM_INPUTCHANNELPOLARITY_FALLING;

  if (HAL_TIM_IC_ConfigChannel(&htim2, &capture, TIM_CHANNEL_1) != HAL_OK) {
    Error_Handler();
  }

  /* Existing reward/event input remains active-high. */
  capture.ICPolarity = TIM_INPUTCHANNELPOLARITY_RISING;

  if (HAL_TIM_IC_ConfigChannel(&htim2, &capture, TIM_CHANNEL_2) != HAL_OK) {
    Error_Handler();
  }

  HAL_NVIC_SetPriority(TIM2_IRQn, 0U, 0U);
  HAL_NVIC_EnableIRQ(TIM2_IRQn);

  __HAL_TIM_SET_COUNTER(&htim2, 0U);
  __HAL_TIM_CLEAR_FLAG(&htim2, TIM_FLAG_UPDATE | TIM_FLAG_CC1 | TIM_FLAG_CC2 |
                                   TIM_FLAG_CC1OF | TIM_FLAG_CC2OF);

  if (HAL_TIM_IC_Start(&htim2, TIM_CHANNEL_1) != HAL_OK) {
    Error_Handler();
  }

  if (HAL_TIM_IC_Start(&htim2, TIM_CHANNEL_2) != HAL_OK) {
    Error_Handler();
  }

  __HAL_TIM_ENABLE_IT(&htim2, TIM_IT_UPDATE | TIM_IT_CC1 | TIM_IT_CC2);
}

/* --------------------------- TIM2 IRQ ------------------------------------ */

void TIM2_IRQHandler(void) {
  const uint32_t status = TIM2->SR;
  const uint32_t captured_ch1 = TIM2->CCR1;
  const uint32_t captured_ch2 = TIM2->CCR2;
  uint32_t flags_to_clear = 0U;

  const bool sync_captured = (status & TIM_SR_CC1IF) != 0U;
  const bool event_captured = (status & TIM_SR_CC2IF) != 0U;
  const bool sync_overcapture = (status & TIM_SR_CC1OF) != 0U;
  const bool event_overcapture = (status & TIM_SR_CC2OF) != 0U;

  uint64_t sync_ticks = 0ULL;
  uint64_t event_ticks = 0ULL;

  if (sync_captured) {
    sync_ticks = extend_capture_value(captured_ch1, status);
    flags_to_clear |= TIM_SR_CC1IF;
  }

  if (event_captured) {
    event_ticks = extend_capture_value(captured_ch2, status);
    flags_to_clear |= TIM_SR_CC2IF;
  }

  if (sync_overcapture) {
    capture_loss_count++;
    latched_status_flags |=
        REWARD_STATUS_CAPTURE_LOSS_LATCHED | REWARD_STATUS_CLOCK_FAULT;
    flags_to_clear |= TIM_SR_CC1OF;
  }

  if (event_overcapture) {
    capture_loss_count++;
    latched_status_flags |=
        REWARD_STATUS_CAPTURE_LOSS_LATCHED | REWARD_STATUS_CLOCK_FAULT;
    flags_to_clear |= TIM_SR_CC2OF;
  }

  if (sync_captured && event_captured) {
    if (sync_ticks <= event_ticks) {
      queue_capture_locked(REWARD_RECORD_SYNC, sync_ticks,
                           sync_overcapture ? REWARD_RECORD_FLAG_HW_OVERCAPTURE
                                            : 0U);
      queue_capture_locked(REWARD_RECORD_EVENT, event_ticks,
                           event_overcapture ? REWARD_RECORD_FLAG_HW_OVERCAPTURE
                                             : 0U);
    } else {
      queue_capture_locked(REWARD_RECORD_EVENT, event_ticks,
                           event_overcapture ? REWARD_RECORD_FLAG_HW_OVERCAPTURE
                                             : 0U);
      queue_capture_locked(REWARD_RECORD_SYNC, sync_ticks,
                           sync_overcapture ? REWARD_RECORD_FLAG_HW_OVERCAPTURE
                                            : 0U);
    }
  } else if (sync_captured) {
    queue_capture_locked(REWARD_RECORD_SYNC, sync_ticks,
                         sync_overcapture ? REWARD_RECORD_FLAG_HW_OVERCAPTURE
                                          : 0U);
  } else if (event_captured) {
    queue_capture_locked(REWARD_RECORD_EVENT, event_ticks,
                         event_overcapture ? REWARD_RECORD_FLAG_HW_OVERCAPTURE
                                           : 0U);
  }

  if ((status & TIM_SR_UIF) != 0U) {
    timer_overflow_high++;
    flags_to_clear |= TIM_SR_UIF;
  }

  if (flags_to_clear != 0U) {
    TIM2->SR = ~flags_to_clear;
  }

  __DMB();
}

/* ----------------------------- RS-485 USART1 ---------------------------- */

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *uart) {
  if (uart->Instance != USART1) {
    return;
  }

  for (size_t index = 0U; index < sizeof(rs485_rx_block); ++index) {
    rs485_rx_byte_count++;
    rs485_consume_byte_from_isr(rs485_rx_block[index]);
  }

  if (HAL_UART_Receive_IT(&huart1, rs485_rx_block, sizeof(rs485_rx_block)) !=
      HAL_OK) {
    transport_error_count++;
    latched_status_flags |= REWARD_STATUS_TRANSPORT_ERROR_LATCHED;
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *uart) {
  if (uart->Instance != USART1) {
    return;
  }

  const uint32_t error = uart->ErrorCode;

  if ((error & HAL_UART_ERROR_FE) != 0U) {
    rs485_framing_error_count++;
  }

  if ((error & HAL_UART_ERROR_NE) != 0U) {
    rs485_noise_error_count++;
  }

  if ((error & HAL_UART_ERROR_ORE) != 0U) {
    rs485_overrun_error_count++;
  }

  if ((error & HAL_UART_ERROR_PE) != 0U) {
    rs485_parity_error_count++;
  }

  transport_error_count++;
  latched_status_flags |= REWARD_STATUS_TRANSPORT_ERROR_LATCHED;

  /*
   * Discard any partial request. The magic-byte parser will resynchronise
   * when reception resumes.
   */
  rs485_request_index = 0U;

  __HAL_UART_CLEAR_OREFLAG(uart);
  __HAL_UART_CLEAR_FEFLAG(uart);
  __HAL_UART_CLEAR_NEFLAG(uart);
  __HAL_UART_CLEAR_PEFLAG(uart);

  (void)HAL_UART_Receive_IT(&huart1, rs485_rx_block, sizeof(rs485_rx_block));
}

void SysTick_Handler(void) { HAL_IncTick(); }

void USART1_IRQHandler(void) { HAL_UART_IRQHandler(&huart1); }

static void Error_Handler(void) {
  __disable_irq();
  latched_status_flags |= REWARD_STATUS_CLOCK_FAULT;

  while (1) {
    /* Fatal initialization failure. */
  }
}
