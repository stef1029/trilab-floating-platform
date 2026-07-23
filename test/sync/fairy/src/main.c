/*
 * NUCLEO-G071RB reward-port timing target
 *
 * Build: PlatformIO + STM32Cube HAL
 * Board: nucleo_g071rb
 *
 * Connections:
 *   PB8 / D15  <-  Korora I2C SCL
 *   PB9 / D14  <-> Korora I2C SDA
 *   PA0 / A0   <-  Korora SYNC pulse (TIM2_CH1)
 *   PA1 / A1   <-  reward/event input (TIM2_CH2)
 *   GND        --- common ground
 *
 * Design:
 *   - TIM2 is a free-running 16 MHz clock and is never reset after startup.
 *   - SYNC, EVENT, and COMMAND_ACK records share one FIFO.
 *   - A completed 40-byte I2C read pops exactly one FIFO record.
 *   - A one-byte I2C write with bit 7 set executes an immediate command.
 *   - Command execution and ACK creation happen in the I2C completion path.
 *   - Debug UART is interrupt-driven and never blocks functional firmware.
 */

#include "stm32g0xx_hal.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define I2C_TARGET_ADDRESS 0x42U

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
#define FRAME_OFFSET_I2C_ERROR_COUNT 28U
#define FRAME_OFFSET_AUXILIARY 32U
#define FRAME_OFFSET_CRC 36U

#define REWARD_STATUS_RECORD_VALID (1U << 0)
#define REWARD_STATUS_FIRST_AFTER_RESET (1U << 1)
#define REWARD_STATUS_CAPTURE_LOSS_LATCHED (1U << 2)
#define REWARD_STATUS_I2C_ERROR_LATCHED (1U << 3)
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

I2C_HandleTypeDef hi2c1;
TIM_HandleTypeDef htim2;
UART_HandleTypeDef huart2;

static volatile uint32_t timer_overflow_high;
static volatile uint32_t capture_loss_count;
static volatile uint32_t i2c_error_count;
static volatile uint8_t latched_status_flags;
static uint32_t reset_cause;

static volatile struct capture_record capture_queue[CAPTURE_QUEUE_CAPACITY];
static volatile uint8_t capture_queue_head;
static volatile uint8_t capture_queue_tail;
static volatile uint8_t capture_queue_count;

static uint8_t i2c_tx_frame[REWARD_FRAME_SIZE];
static volatile bool i2c_tx_frame_has_record;
static volatile uint64_t i2c_tx_record_ticks;
static volatile uint8_t i2c_tx_record_type;

static volatile uint8_t i2c_rx_command;
static volatile bool i2c_command_rx_armed;
static volatile uint32_t i2c_rx_callback_count;
static volatile uint32_t i2c_listen_fallback_count;

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

static void SystemClock_Config(void);
static void GPIO_Init(void);
static void USART2_Init(void);
static void TIM2_Init(void);
static void I2C1_Init(void);
static void Error_Handler(void);
static void prepare_i2c_snapshot(void);
static void service_command_debug(void);
static void service_uart_heartbeat(void);

/*
 * Needs to be replaced with a real do_something_now function
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

  debug_uart_logf("UART_ALIVE,%lu,DROPPED,%lu,I2C_ERRORS,%lu\r\n",
                  (unsigned long)heartbeat_index,
                  (unsigned long)debug_uart_dropped_bytes,
                  (unsigned long)i2c_error_count);
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

/* -------------------------- Command handling ------------------------------ */

static void process_i2c_command_from_isr(uint8_t command) {
  if (command == REWARD_COMMAND_ACK_RESET_SEEN) {
    const uint32_t saved_primask = __get_PRIMASK();
    __disable_irq();
    latched_status_flags &= (uint8_t)~REWARD_STATUS_FIRST_AFTER_RESET;
    if (saved_primask == 0U) {
      __enable_irq();
    }
    return;
  }

  if (command == REWARD_COMMAND_CLEAR_LATCHED_FLAGS) {
    const uint32_t saved_primask = __get_PRIMASK();
    __disable_irq();
    latched_status_flags &=
        (uint8_t)~(REWARD_STATUS_CAPTURE_LOSS_LATCHED |
                   REWARD_STATUS_I2C_ERROR_LATCHED | REWARD_STATUS_CLOCK_FAULT);
    if (saved_primask == 0U) {
      __enable_irq();
    }
    return;
  }

  if ((command & REWARD_COMMAND_DO_NOW_MASK) == 0U) {
    return;
  }

  const uint8_t token = command & REWARD_COMMAND_TOKEN_MASK;
  uint64_t received_ticks;
  uint64_t completed_ticks;
  bool queued;

  uint32_t saved_primask = __get_PRIMASK();
  __disable_irq();
  received_ticks = timer_snapshot_locked();
  if (saved_primask == 0U) {
    __enable_irq();
  }

  reward_do_something_now_isr();

  saved_primask = __get_PRIMASK();
  __disable_irq();

  completed_ticks = timer_snapshot_locked();
  queued = queue_command_ack_locked(token, received_ticks, completed_ticks);

  command_debug_token = token;
  command_debug_received_ticks = received_ticks;
  command_debug_completed_ticks = completed_ticks;
  command_debug_queued = queued;
  __DMB();
  command_debug_pending = true;

  if (saved_primask == 0U) {
    __enable_irq();
  }
}

static void complete_i2c_command_receive_from_isr(bool from_fallback) {
  if (!i2c_command_rx_armed) {
    return;
  }

  i2c_command_rx_armed = false;

  if (from_fallback) {
    i2c_listen_fallback_count++;
  } else {
    i2c_rx_callback_count++;
  }

  process_i2c_command_from_isr(i2c_rx_command);
}

static void service_command_debug(void) {
  uint8_t token;
  bool queued;
  uint64_t received_ticks;
  uint64_t completed_ticks;
  uint32_t callback_count;
  uint32_t fallback_count;

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
  callback_count = i2c_rx_callback_count;
  fallback_count = i2c_listen_fallback_count;
  command_debug_pending = false;

  if (saved_primask == 0U) {
    __enable_irq();
  }

  debug_uart_logf("REWARD_COMMAND,%u,%u,%llu,%llu,RX_CB,%lu,FALLBACK,%lu\r\n",
                  token, queued ? 1U : 0U, (unsigned long long)received_ticks,
                  (unsigned long long)completed_ticks,
                  (unsigned long)callback_count, (unsigned long)fallback_count);
}

/* -------------------------- I2C frame ------------------------------------ */

static void prepare_i2c_snapshot(void) {
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
  error_count = i2c_error_count;

  i2c_tx_frame_has_record = has_record;
  i2c_tx_record_ticks = record.ticks;
  i2c_tx_record_type = record.type;

  if (saved_primask == 0U) {
    __enable_irq();
  }

  memset(i2c_tx_frame, 0, sizeof(i2c_tx_frame));

  i2c_tx_frame[FRAME_OFFSET_MAGIC] = REWARD_FRAME_MAGIC;
  i2c_tx_frame[FRAME_OFFSET_VERSION] = REWARD_FRAME_VERSION;
  i2c_tx_frame[FRAME_OFFSET_STATUS_FLAGS] = status_flags;
  i2c_tx_frame[FRAME_OFFSET_LENGTH] = REWARD_FRAME_SIZE;
  i2c_tx_frame[FRAME_OFFSET_RECORD_TYPE] = record.type;
  i2c_tx_frame[FRAME_OFFSET_PENDING_COUNT] = pending_count;
  i2c_tx_frame[FRAME_OFFSET_RECORD_FLAGS] = record.flags;

  put_u64_le(&i2c_tx_frame[FRAME_OFFSET_CAPTURE_TICKS], record.ticks);
  put_u64_le(&i2c_tx_frame[FRAME_OFFSET_SNAPSHOT_TICKS], snapshot_ticks);
  put_u32_le(&i2c_tx_frame[FRAME_OFFSET_CAPTURE_LOSS_COUNT], loss_count);
  put_u32_le(&i2c_tx_frame[FRAME_OFFSET_I2C_ERROR_COUNT], error_count);
  put_u32_le(&i2c_tx_frame[FRAME_OFFSET_AUXILIARY],
             (record.type == REWARD_RECORD_COMMAND_ACK) ? record.auxiliary
                                                        : reset_cause);

  const uint16_t crc = crc16_ccitt(i2c_tx_frame, FRAME_OFFSET_CRC);
  put_u16_le(&i2c_tx_frame[FRAME_OFFSET_CRC], crc);
}

/* ------------------------------ Main ------------------------------------- */

int main(void) {
  reset_cause = RCC->CSR;

  HAL_Init();
  __HAL_RCC_CLEAR_RESET_FLAGS();

  latched_status_flags = REWARD_STATUS_FIRST_AFTER_RESET;

  SystemClock_Config();
  GPIO_Init();
  USART2_Init();
  TIM2_Init();
  I2C1_Init();

  debug_uart_puts("\r\n"
                  "REWARD_BOOT,G071RB\r\n"
                  "REWARD_COMMAND_PROTOCOL,1\r\n");

  if (HAL_I2C_EnableListen_IT(&hi2c1) != HAL_OK) {
    Error_Handler();
  }

  while (1) {
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

  gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1;
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

  gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9;
  gpio.Mode = GPIO_MODE_AF_OD;
  gpio.Pull = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  gpio.Alternate = GPIO_AF6_I2C1;
  HAL_GPIO_Init(GPIOB, &gpio);
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
  huart2.Init.BaudRate = 115200U;
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

  HAL_NVIC_SetPriority(USART2_IRQn, 1U, 0U);
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

  capture.ICPolarity = TIM_INPUTCHANNELPOLARITY_RISING;
  capture.ICSelection = TIM_ICSELECTION_DIRECTTI;
  capture.ICPrescaler = TIM_ICPSC_DIV1;
  capture.ICFilter = 4U;

  if (HAL_TIM_IC_ConfigChannel(&htim2, &capture, TIM_CHANNEL_1) != HAL_OK) {
    Error_Handler();
  }

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

static void I2C1_Init(void) {
  RCC_PeriphCLKInitTypeDef peripheral_clock = {0};

  peripheral_clock.PeriphClockSelection = RCC_PERIPHCLK_I2C1;
  peripheral_clock.I2c1ClockSelection = RCC_I2C1CLKSOURCE_PCLK1;

  if (HAL_RCCEx_PeriphCLKConfig(&peripheral_clock) != HAL_OK) {
    Error_Handler();
  }

  __HAL_RCC_I2C1_CLK_ENABLE();

  hi2c1.Instance = I2C1;
  hi2c1.Init.Timing = 0x00303D5BU;
  hi2c1.Init.OwnAddress1 = I2C_TARGET_ADDRESS << 1;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0U;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;

  if (HAL_I2C_Init(&hi2c1) != HAL_OK) {
    Error_Handler();
  }

  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK) {
    Error_Handler();
  }

  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0U) != HAL_OK) {
    Error_Handler();
  }

  HAL_NVIC_SetPriority(I2C1_IRQn, 2U, 0U);
  HAL_NVIC_EnableIRQ(I2C1_IRQn);
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

/* ----------------------------- I2C --------------------------------------- */

static void latch_i2c_error(void) {
  const uint32_t saved_primask = __get_PRIMASK();
  __disable_irq();

  i2c_error_count++;
  latched_status_flags |= REWARD_STATUS_I2C_ERROR_LATCHED;

  if (saved_primask == 0U) {
    __enable_irq();
  }
}

void HAL_I2C_AddrCallback(I2C_HandleTypeDef *i2c, uint8_t transfer_direction,
                          uint16_t address_match_code) {
  (void)address_match_code;

  if (i2c->Instance != I2C1) {
    return;
  }

  if (transfer_direction == I2C_DIRECTION_TRANSMIT) {
    i2c_rx_command = 0U;
    i2c_command_rx_armed = true;

    if (HAL_I2C_Slave_Seq_Receive_IT(i2c, (uint8_t *)&i2c_rx_command, 1U,
                                     I2C_FIRST_AND_LAST_FRAME) != HAL_OK) {
      i2c_command_rx_armed = false;
      latch_i2c_error();
    }

    return;
  }

  prepare_i2c_snapshot();

  if (HAL_I2C_Slave_Seq_Transmit_IT(i2c, i2c_tx_frame, REWARD_FRAME_SIZE,
                                    I2C_FIRST_AND_LAST_FRAME) != HAL_OK) {
    i2c_tx_frame_has_record = false;
    latch_i2c_error();
  }
}

void HAL_I2C_SlaveRxCpltCallback(I2C_HandleTypeDef *i2c) {
  if (i2c->Instance == I2C1) {
    complete_i2c_command_receive_from_isr(false);
  }
}

void HAL_I2C_SlaveTxCpltCallback(I2C_HandleTypeDef *i2c) {
  if ((i2c->Instance != I2C1) || !i2c_tx_frame_has_record) {
    return;
  }

  const uint32_t saved_primask = __get_PRIMASK();
  __disable_irq();

  const bool popped =
      pop_expected_capture_locked(i2c_tx_record_type, i2c_tx_record_ticks);

  i2c_tx_frame_has_record = false;

  if (!popped) {
    latched_status_flags |= REWARD_STATUS_CLOCK_FAULT;
  }

  if (saved_primask == 0U) {
    __enable_irq();
  }
}

void HAL_I2C_ListenCpltCallback(I2C_HandleTypeDef *i2c) {
  if (i2c->Instance != I2C1) {
    return;
  }

  if (i2c_command_rx_armed) {
    if (i2c->XferCount == 0U) {
      complete_i2c_command_receive_from_isr(true);
    } else {
      i2c_command_rx_armed = false;
      latch_i2c_error();
    }
  }

  if (HAL_I2C_EnableListen_IT(i2c) != HAL_OK) {
    latch_i2c_error();
  }
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *i2c) {
  if (i2c->Instance != I2C1) {
    return;
  }

  const uint32_t error = HAL_I2C_GetError(i2c);

  if ((error & HAL_I2C_ERROR_AF) != 0U) {
    __HAL_I2C_CLEAR_FLAG(i2c, I2C_FLAG_AF);
  }

  if ((error & ~HAL_I2C_ERROR_AF) != 0U) {
    i2c_tx_frame_has_record = false;
    i2c_command_rx_armed = false;
    latch_i2c_error();
  }

  if (HAL_I2C_GetState(i2c) == HAL_I2C_STATE_READY) {
    if (HAL_I2C_EnableListen_IT(i2c) != HAL_OK) {
      latch_i2c_error();
    }
  }
}

void I2C1_IRQHandler(void) {
  HAL_I2C_EV_IRQHandler(&hi2c1);
  HAL_I2C_ER_IRQHandler(&hi2c1);
}

static void Error_Handler(void) {
  __disable_irq();
  latched_status_flags |= REWARD_STATUS_CLOCK_FAULT;

  while (1) {
    /* Fatal initialization failure. */
  }
}