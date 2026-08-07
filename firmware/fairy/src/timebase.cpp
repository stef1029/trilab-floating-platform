#include "timebase.hpp"

#include <array>

#include <stm32g0xx_hal.h>

#include "board_profile.hpp"

namespace fairy_timebase {
namespace {

TIM_HandleTypeDef timer;
volatile std::uint32_t high_word;
std::array<Capture, 32> captures;
volatile std::uint8_t capture_read;
volatile std::uint8_t capture_write;
volatile bool capture_loss_latched;

void push_capture(CaptureKind kind, std::uint32_t low, bool overcapture,
                  bool overflow_pending) {
  std::uint32_t high = high_word;
  if (overflow_pending && low < 0x80000000U) {
    ++high;
  }
  const std::uint8_t next =
      static_cast<std::uint8_t>((capture_write + 1U) % captures.size());
  if (next == capture_read) {
    capture_loss_latched = true;
    return;
  }
  captures[capture_write] =
      Capture{kind, (static_cast<std::uint64_t>(high) << 32U) | low,
              overcapture || capture_loss_latched};
  capture_loss_latched = false;
  __DMB();
  capture_write = next;
}

} // namespace

void initialize() {
  __HAL_RCC_TIM2_CLK_ENABLE();
  timer.Instance = TIM2;
  timer.Init.Prescaler = 3U;
  timer.Init.CounterMode = TIM_COUNTERMODE_UP;
  timer.Init.Period = 0xFFFFFFFFU;
  timer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  timer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_IC_Init(&timer) != HAL_OK) {
    Error_Handler();
  }

  TIM_IC_InitTypeDef capture{};
  capture.ICSelection = TIM_ICSELECTION_DIRECTTI;
  capture.ICPrescaler = TIM_ICPSC_DIV1;
  capture.ICFilter = 0;
  capture.ICPolarity = TIM_INPUTCHANNELPOLARITY_FALLING;
  if (HAL_TIM_IC_ConfigChannel(&timer, &capture, TIM_CHANNEL_1) != HAL_OK) {
    Error_Handler();
  }
  capture.ICPolarity = TIM_INPUTCHANNELPOLARITY_RISING;
  if (HAL_TIM_IC_ConfigChannel(&timer, &capture, TIM_CHANNEL_2) != HAL_OK) {
    Error_Handler();
  }

  HAL_NVIC_SetPriority(TIM2_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(TIM2_IRQn);
  __HAL_TIM_SET_COUNTER(&timer, 0);
  __HAL_TIM_CLEAR_FLAG(&timer, TIM_FLAG_UPDATE | TIM_FLAG_CC1 | TIM_FLAG_CC2 |
                                   TIM_FLAG_CC1OF | TIM_FLAG_CC2OF);
  (void)HAL_TIM_IC_Start(&timer, TIM_CHANNEL_1);
  (void)HAL_TIM_IC_Start(&timer, TIM_CHANNEL_2);
  __HAL_TIM_ENABLE_IT(&timer, TIM_IT_UPDATE | TIM_IT_CC1 | TIM_IT_CC2);
}

std::uint64_t now() {
  const std::uint32_t primask = __get_PRIMASK();
  __disable_irq();
  std::uint32_t high = high_word;
  std::uint32_t low = TIM2->CNT;
  if ((TIM2->SR & TIM_SR_UIF) != 0U && low < 0x80000000U) {
    ++high;
    low = TIM2->CNT;
  }
  if (primask == 0U) {
    __enable_irq();
  }
  return (static_cast<std::uint64_t>(high) << 32U) | low;
}

bool pop_capture(Capture &capture) {
  const std::uint32_t primask = __get_PRIMASK();
  __disable_irq();
  if (capture_read == capture_write) {
    if (primask == 0U) {
      __enable_irq();
    }
    return false;
  }
  capture = captures[capture_read];
  capture_read =
      static_cast<std::uint8_t>((capture_read + 1U) % captures.size());
  if (primask == 0U) {
    __enable_irq();
  }
  return true;
}

void busy_wait_us(std::uint32_t microseconds) {
  const std::uint32_t ticks = microseconds * 16U;
  const std::uint32_t start = TIM2->CNT;
  while (static_cast<std::uint32_t>(TIM2->CNT - start) < ticks) {
  }
}

extern "C" void TIM2_IRQHandler() {
  const std::uint32_t status = TIM2->SR;
  const bool overflow = (status & TIM_SR_UIF) != 0U;
  const std::uint32_t sync = TIM2->CCR1;
  const std::uint32_t light = TIM2->CCR2;

  if ((status & TIM_SR_CC1IF) != 0U) {
    push_capture(CaptureKind::sync, sync, (status & TIM_SR_CC1OF) != 0U,
                 overflow);
  }
  if ((status & TIM_SR_CC2IF) != 0U) {
    push_capture(CaptureKind::light_gate, light, (status & TIM_SR_CC2OF) != 0U,
                 overflow);
  }
  if (overflow) {
    ++high_word;
  }
  /*
   * Clear only flags seen at ISR entry. A new edge that arrives while this
   * handler runs therefore remains pending for the next interrupt.
   */
  TIM2->SR = ~status;
}

} // namespace fairy_timebase
