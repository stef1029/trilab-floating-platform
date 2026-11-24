#include <cstdint>

#include <stm32g0xx_hal.h>

#include "application.hpp"
#include "board_profile.hpp"
#include "debug_log.hpp"
#include "rig_protocol/command_protocol.hpp"
#include "rig_protocol/record_protocol.hpp"
#include "rig_protocol/tlv.hpp"
#include "light_sensor.hpp"
#include "outputs.hpp"
#include "record_store.hpp"
#include "rs485_link.hpp"
#include "timebase.hpp"

namespace {

IWDG_HandleTypeDef watchdog;
const char *current_boot_stage = "reset";
bool debug_ready;

void report_boot_stage(const char *stage) {
  current_boot_stage = stage;
  if (debug_ready) {
    node_debug::log("BOOT_STAGE %s\r\n", stage);
    node_debug::service();
  }
}

void system_clock_initialize() {
  RCC_OscInitTypeDef oscillator{};
  oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  oscillator.HSIState = RCC_HSI_ON;
  oscillator.HSIDiv = RCC_HSI_DIV1;
  oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  oscillator.PLL.PLLState = RCC_PLL_ON;
  oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  oscillator.PLL.PLLM = RCC_PLLM_DIV1;
  oscillator.PLL.PLLN = 8;
  oscillator.PLL.PLLP = RCC_PLLP_DIV2;
  oscillator.PLL.PLLQ = RCC_PLLQ_DIV2;
  oscillator.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) {
    Error_Handler();
  }

  RCC_ClkInitTypeDef clock{};
  clock.ClockType =
      RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1;
  clock.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clock.APB1CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_2) != HAL_OK) {
    Error_Handler();
  }
}

void watchdog_initialize() {
  __HAL_RCC_LSI_ENABLE();
  while (__HAL_RCC_GET_FLAG(RCC_FLAG_LSIRDY) == RESET) {
  }
  watchdog.Instance = IWDG;
  watchdog.Init.Prescaler = IWDG_PRESCALER_32;
  watchdog.Init.Window = IWDG_WINDOW_DISABLE;
  watchdog.Init.Reload = 1000;
  if (HAL_IWDG_Init(&watchdog) != HAL_OK) {
    Error_Handler();
  }
}

} // namespace

int main() {
  const std::uint32_t reset_cause = RCC->CSR;
  HAL_Init();
  system_clock_initialize();
  __HAL_RCC_CLEAR_RESET_FLAGS();

  node_board::initialize_safe_gpio();
  node_debug::initialize();
  debug_ready = true;

  node_debug::log(
      "NODE_BUILD profile=%s timer_hz=16000000 rs485_baud=460800\r\n",
      node_board::name);
  node_debug::service();

  report_boot_stage("record_store");
  node_records::initialize();

  report_boot_stage("alternate_functions");
  node_board::initialize_alternate_functions();

  report_boot_stage("timebase");
  node_timebase::initialize();

  /*
   * Communications are initialized before optional measurement and output
   * peripherals. Incoming bytes are buffered by the USART interrupt while
   * the remaining short self tests complete.
   */
  report_boot_stage("rs485");
  node_rs485::initialize();

  report_boot_stage("outputs");
  node_outputs::initialize();

  report_boot_stage("light_sensor");
  const bool light_initialized = node_light::initialize();
  const node_light::SelfTest light_test =
      light_initialized
          ? node_light::self_test()
          : node_light::SelfTest{0xFFFFU, 0xFFFFU, false, 0, 0, true};

  report_boot_stage("application");
  node_application::initialize(reset_cause, light_test.passed, light_test.dark,
                                light_test.clear);

  report_boot_stage("watchdog");
  watchdog_initialize();

  report_boot_stage("ready");
  node_debug::log(
      "NODE_READY light_initialized=%u light_passed=%u dark=%u clear=%u "
      "gate_clear=%u gate_blocked=%u gate_fixed=%u\r\n",
      light_initialized ? 1U : 0U, light_test.passed ? 1U : 0U,
      static_cast<unsigned int>(light_test.dark),
      static_cast<unsigned int>(light_test.clear),
      static_cast<unsigned int>(light_test.clear_threshold),
      static_cast<unsigned int>(light_test.blocked_threshold),
      light_test.fixed_thresholds ? 1U : 0U);
  node_debug::service();

  while (true) {
    node_rs485::service();
    node_application::service();
    node_debug::service();
    (void)HAL_IWDG_Refresh(&watchdog);
    __WFI();
  }
}

extern "C" void SysTick_Handler() { HAL_IncTick(); }

extern "C" void Error_Handler() {
  if (debug_ready) {
    node_debug::log("FATAL stage=%s\r\n", current_boot_stage);
    node_debug::service();
  }
  __disable_irq();
  node_board::initialize_safe_gpio();
  while (true) {
  }
}
