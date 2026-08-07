#include <cstdint>

#include <stm32g0xx_hal.h>

#include "application.hpp"
#include "board_profile.hpp"
#include "debug_log.hpp"
#include "fairy_shared/adelie_protocol.hpp"
#include "fairy_shared/fairy_protocol.hpp"
#include "fairy_shared/tlv.hpp"
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
    fairy_debug::log("BOOT_STAGE %s\r\n", stage);
    fairy_debug::service();
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

  fairy_board::initialize_safe_gpio();
  fairy_debug::initialize();
  debug_ready = true;

  fairy_debug::log(
      "FAIRY_BUILD profile=%s timer_hz=16000000 rs485_baud=460800\r\n",
      fairy_board::name);
  fairy_debug::service();

  report_boot_stage("record_store");
  fairy_records::initialize();

  report_boot_stage("alternate_functions");
  fairy_board::initialize_alternate_functions();

  report_boot_stage("timebase");
  fairy_timebase::initialize();

  /*
   * Communications are initialized before optional measurement and output
   * peripherals. Incoming bytes are buffered by the USART interrupt while
   * the remaining short self tests complete.
   */
  report_boot_stage("rs485");
  fairy_rs485::initialize();

  report_boot_stage("outputs");
  fairy_outputs::initialize();

  report_boot_stage("light_sensor");
  const bool light_initialized = fairy_light::initialize();
  const fairy_light::SelfTest light_test =
      light_initialized
          ? fairy_light::self_test()
          : fairy_light::SelfTest{0xFFFFU, 0xFFFFU, false, 0, 0, true};

  report_boot_stage("application");
  fairy_application::initialize(reset_cause, light_test.passed, light_test.dark,
                                light_test.clear);

  report_boot_stage("watchdog");
  watchdog_initialize();

  report_boot_stage("ready");
  fairy_debug::log(
      "FAIRY_READY light_initialized=%u light_passed=%u dark=%u clear=%u "
      "gate_clear=%u gate_blocked=%u gate_fixed=%u\r\n",
      light_initialized ? 1U : 0U, light_test.passed ? 1U : 0U,
      static_cast<unsigned int>(light_test.dark),
      static_cast<unsigned int>(light_test.clear),
      static_cast<unsigned int>(light_test.clear_threshold),
      static_cast<unsigned int>(light_test.blocked_threshold),
      light_test.fixed_thresholds ? 1U : 0U);
  fairy_debug::service();

  while (true) {
    fairy_rs485::service();
    fairy_application::service();
    fairy_debug::service();
    (void)HAL_IWDG_Refresh(&watchdog);
    __WFI();
  }
}

extern "C" void SysTick_Handler() { HAL_IncTick(); }

extern "C" void Error_Handler() {
  if (debug_ready) {
    fairy_debug::log("FATAL stage=%s\r\n", current_boot_stage);
    fairy_debug::service();
  }
  __disable_irq();
  fairy_board::initialize_safe_gpio();
  while (true) {
  }
}
