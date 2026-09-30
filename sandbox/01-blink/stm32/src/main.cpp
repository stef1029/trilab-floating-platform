/*
 * Blink LD4 (the green user LED) on a NUCLEO-G071RB, and print to the
 * ST-Link virtual COM port at 115200 baud.
 *
 * LD4 is on PA5. Unlike the Zephyr version, nothing looks that up for us --
 * on STM32 the pin is named in the code.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <stm32g0xx_hal.h>

extern "C" void Error_Handler();

namespace {

constexpr std::uint32_t blink_interval_ms = 500;

/* LD4 user LED */
GPIO_TypeDef *const led_port = GPIOA;
constexpr std::uint16_t led_pin = GPIO_PIN_5;

/* USART2 is wired to the ST-Link virtual COM port on this board. */
UART_HandleTypeDef uart;

void system_clock_initialize() {
  RCC_OscInitTypeDef oscillator{};
  oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  oscillator.HSIState = RCC_HSI_ON;
  oscillator.HSIDiv = RCC_HSI_DIV1;
  oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  oscillator.PLL.PLLState = RCC_PLL_ON;
  oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  oscillator.PLL.PLLM = RCC_PLLM_DIV1;
  oscillator.PLL.PLLN = 8;              /* 16 MHz x 8 / 2 = 64 MHz */
  oscillator.PLL.PLLP = RCC_PLLP_DIV2;
  oscillator.PLL.PLLQ = RCC_PLLQ_DIV2;
  oscillator.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) {
    Error_Handler();
  }

  RCC_ClkInitTypeDef clock{};
  clock.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1;
  clock.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clock.APB1CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_2) != HAL_OK) {
    Error_Handler();
  }
}

void led_initialize() {
  __HAL_RCC_GPIOA_CLK_ENABLE();
  /* Value first, then mode -- see H1. */
  HAL_GPIO_WritePin(led_port, led_pin, GPIO_PIN_RESET);
  GPIO_InitTypeDef gpio{};
  gpio.Pin = led_pin;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(led_port, &gpio);
}

void console_initialize() {
  __HAL_RCC_USART2_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  GPIO_InitTypeDef gpio{};
  gpio.Pin = GPIO_PIN_2 | GPIO_PIN_3;   /* PA2 = TX, PA3 = RX */
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF1_USART2;
  HAL_GPIO_Init(GPIOA, &gpio);

  uart.Instance = USART2;
  uart.Init.BaudRate = 115200;
  uart.Init.WordLength = UART_WORDLENGTH_8B;
  uart.Init.StopBits = UART_STOPBITS_1;
  uart.Init.Parity = UART_PARITY_NONE;
  uart.Init.Mode = UART_MODE_TX_RX;
  uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  uart.Init.OverSampling = UART_OVERSAMPLING_16;
  uart.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&uart) != HAL_OK) {
    Error_Handler();
  }
}

void print(const char *text) {
  (void)HAL_UART_Transmit(&uart, reinterpret_cast<const std::uint8_t *>(text),
                          static_cast<std::uint16_t>(std::strlen(text)), 100);
}

} // namespace

int main() {
  HAL_Init();
  system_clock_initialize();
  led_initialize();
  console_initialize();

  print("\r\nblink-stm32 running: LD4 on PA5, every 500 ms\r\n");

  std::uint32_t count = 0;
  while (true) {
    HAL_GPIO_TogglePin(led_port, led_pin);

    char line[48];
    (void)std::snprintf(line, sizeof(line), "toggle %lu\r\n",
                        static_cast<unsigned long>(count++));
    print(line);

    /* Busy-waits on the SysTick counter -- no scheduler to yield to. */
    HAL_Delay(blink_interval_ms);
  }
}

extern "C" void SysTick_Handler() { HAL_IncTick(); }

extern "C" void Error_Handler() {
  __disable_irq();
  while (true) {
  }
}
