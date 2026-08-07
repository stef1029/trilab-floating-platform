#include "board_profile.hpp"

namespace fairy_board {

void enable_gpio_clocks() {
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
}

void initialize_safe_gpio() {
  enable_gpio_clocks();
  GPIO_InitTypeDef gpio{};

  if constexpr (prototype) {
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0 | GPIO_PIN_1, GPIO_PIN_RESET);
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_PULLDOWN;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);
  } else {
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2 | GPIO_PIN_3, GPIO_PIN_RESET);
    gpio.Pin = GPIO_PIN_2 | GPIO_PIN_3;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_PULLDOWN;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &gpio);

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3, GPIO_PIN_RESET);
    gpio.Pin = GPIO_PIN_3;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_PULLDOWN;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);
  }

  /*
   * Force PWM controlled pins low before their timers claim the pins.
   * Low means valve off and the low side RGB MOSFETs off.
   */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8 | GPIO_PIN_9, GPIO_PIN_RESET);
  gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_PULLDOWN;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &gpio);

  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0 | GPIO_PIN_4 | GPIO_PIN_5,
                   GPIO_PIN_RESET);
  gpio.Pin = GPIO_PIN_0 | GPIO_PIN_4 | GPIO_PIN_5;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_PULLDOWN;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &gpio);
}

void initialize_alternate_functions() {
  GPIO_InitTypeDef gpio{};

  gpio.Pin = GPIO_PIN_0;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_PULLUP;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF2_TIM2;
  HAL_GPIO_Init(GPIOA, &gpio);

  gpio.Pin = GPIO_PIN_1;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &gpio);

  gpio.Pin = GPIO_PIN_4;
  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &gpio);

  gpio.Pin = GPIO_PIN_6;
  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &gpio);

  gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_PULLDOWN;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF2_TIM1;
  HAL_GPIO_Init(GPIOA, &gpio);

  gpio.Pin = GPIO_PIN_0 | GPIO_PIN_4 | GPIO_PIN_5;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_PULLDOWN;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF1_TIM3;
  HAL_GPIO_Init(GPIOB, &gpio);

  if constexpr (prototype) {
    gpio.Pin = GPIO_PIN_4;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF1_USART1;
    HAL_GPIO_Init(GPIOC, &gpio);

    gpio.Pin = GPIO_PIN_5;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF1_USART1;
    HAL_GPIO_Init(GPIOC, &gpio);
  } else {
    gpio.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF0_USART1;
    HAL_GPIO_Init(GPIOB, &gpio);
  }
}

void set_rs485_transmit(bool enabled) {
  if constexpr (final_pcb) {
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3,
                      enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
  } else {
    (void)enabled;
  }
}

void set_amplifier_enabled(bool enabled) {
  if constexpr (prototype) {
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0,
                      enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
  } else {
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2,
                      enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
  }
}

void set_ir_enabled(bool enabled) {
  if constexpr (prototype) {
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_1,
                      enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
  } else {
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3,
                      enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
  }
}

}  // namespace fairy_board
