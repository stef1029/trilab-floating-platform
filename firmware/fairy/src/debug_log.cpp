#include "debug_log.hpp"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <stm32g0xx_hal.h>

#include "fairy_shared/static_queue.hpp"
#include "fairy_shared/system_config.hpp"

namespace fairy_debug {
namespace {

struct Line {
  std::array<char, 160> text{};
  std::uint16_t length{};
};

fairy::StaticQueue<Line, 16> lines;
UART_HandleTypeDef uart;

} // namespace

void initialize() {
#if FAIRY_ENABLE_DEBUG_STREAM
  __HAL_RCC_USART2_CLK_ENABLE();
  GPIO_InitTypeDef gpio{};
  gpio.Pin = GPIO_PIN_2 | GPIO_PIN_3;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF1_USART2;
  HAL_GPIO_Init(GPIOA, &gpio);

  uart.Instance = USART2;
  uart.Init.BaudRate = 460800;
  uart.Init.WordLength = UART_WORDLENGTH_8B;
  uart.Init.StopBits = UART_STOPBITS_1;
  uart.Init.Parity = UART_PARITY_NONE;
  uart.Init.Mode = UART_MODE_TX_RX;
  uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  uart.Init.OverSampling = UART_OVERSAMPLING_16;
  uart.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  (void)HAL_UART_Init(&uart);
#endif
}

void log(const char *format, ...) {
#if FAIRY_ENABLE_DEBUG_STREAM
  if (format == nullptr || lines.full()) {
    return;
  }
  Line line;
  va_list arguments;
  va_start(arguments, format);
  const int result =
      std::vsnprintf(line.text.data(), line.text.size(), format, arguments);
  va_end(arguments);
  if (result <= 0) {
    return;
  }
  line.length = static_cast<std::uint16_t>(
      result >= static_cast<int>(line.text.size()) ? line.text.size() - 1U
                                                   : result);
  (void)lines.push(line);
#else
  (void)format;
#endif
}

void service() {
#if FAIRY_ENABLE_DEBUG_STREAM
  Line line;
  if (!lines.pop(line)) {
    return;
  }
  (void)HAL_UART_Transmit(&uart,
                          reinterpret_cast<std::uint8_t *>(line.text.data()),
                          line.length, 20);
#endif
}

} // namespace fairy_debug
