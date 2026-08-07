#include "light_sensor.hpp"

#include <stm32g0xx_hal.h>

#include "board_profile.hpp"
#include "debug_log.hpp"
#include "timebase.hpp"

namespace fairy_light {
namespace {

ADC_HandleTypeDef adc;
bool ready;
bool monitor_enabled;
bool gate_blocked;
bool event_pending;
std::uint64_t next_sample_ticks;
std::uint64_t next_status_ticks;
GateEvent pending_event;
std::uint16_t latest_value = 0xFFFFU;
std::uint16_t clear_threshold = fairy_board::light_gate_clear_adc;
std::uint16_t blocked_threshold = fairy_board::light_gate_blocked_adc;
std::uint32_t capture_count;
std::uint32_t accepted_count;
std::uint32_t rejected_count;
std::uint32_t adc_failure_count;
std::uint32_t adc_event_count;

inline constexpr std::uint64_t sample_interval_ticks =
    static_cast<std::uint64_t>(fairy_board::light_gate_sample_interval_us) *
    16ULL;
inline constexpr std::uint64_t status_interval_ticks = 16'000'000ULL;
inline constexpr std::uint32_t calibration_settle_us = 5000U;
inline constexpr std::uint32_t calibration_sample_spacing_us = 250U;
inline constexpr std::uint8_t calibration_samples = 8U;

unsigned long ticks_high(std::uint64_t ticks) {
  return static_cast<unsigned long>(ticks >> 32U);
}

unsigned long ticks_low(std::uint64_t ticks) {
  return static_cast<unsigned long>(ticks & 0xFFFFFFFFULL);
}

std::uint16_t averaged_reading() {
  std::uint32_t total = 0U;
  for (std::uint8_t sample = 0; sample < calibration_samples; ++sample) {
    const std::uint16_t value = read();
    if (value == 0xFFFFU) {
      return 0xFFFFU;
    }
    total += value;
    if (sample + 1U != calibration_samples) {
      fairy_timebase::busy_wait_us(calibration_sample_spacing_us);
    }
  }
  return static_cast<std::uint16_t>(total / calibration_samples);
}

} // namespace

bool initialize() {
  ready = false;
  monitor_enabled = false;
  gate_blocked = false;
  event_pending = false;
  next_sample_ticks = 0;
  next_status_ticks = 0;
  latest_value = 0xFFFFU;
  clear_threshold = fairy_board::light_gate_clear_adc;
  blocked_threshold = fairy_board::light_gate_blocked_adc;
  capture_count = 0U;
  accepted_count = 0U;
  rejected_count = 0U;
  adc_failure_count = 0U;
  adc_event_count = 0U;
  __HAL_RCC_ADC_CLK_ENABLE();
  adc.Instance = ADC1;
  adc.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  adc.Init.Resolution = ADC_RESOLUTION_12B;
  adc.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  adc.Init.ScanConvMode = ADC_SCAN_DISABLE;
  adc.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  adc.Init.LowPowerAutoWait = DISABLE;
  adc.Init.LowPowerAutoPowerOff = DISABLE;
  adc.Init.ContinuousConvMode = DISABLE;
  adc.Init.NbrOfConversion = 1;
  adc.Init.DiscontinuousConvMode = DISABLE;
  adc.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  adc.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  adc.Init.DMAContinuousRequests = DISABLE;
  adc.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  adc.Init.SamplingTimeCommon1 = ADC_SAMPLETIME_160CYCLES_5;
  adc.Init.SamplingTimeCommon2 = ADC_SAMPLETIME_160CYCLES_5;
  adc.Init.OversamplingMode = DISABLE;
  adc.Init.TriggerFrequencyMode = ADC_TRIGGER_FREQ_HIGH;
  if (HAL_ADC_Init(&adc) != HAL_OK) {
    fairy_debug::log("LIGHT_GATE_INIT status=adc_init_failed\r\n");
    return false;
  }
  ADC_ChannelConfTypeDef channel{};
  channel.Channel = ADC_CHANNEL_6;
  channel.Rank = ADC_REGULAR_RANK_1;
  channel.SamplingTime = ADC_SAMPLINGTIME_COMMON_1;
  if (HAL_ADC_ConfigChannel(&adc, &channel) != HAL_OK) {
    fairy_debug::log(
        "LIGHT_GATE_INIT status=channel_config_failed channel=6\r\n");
    return false;
  }
  if (HAL_ADCEx_Calibration_Start(&adc) != HAL_OK) {
    fairy_debug::log("LIGHT_GATE_INIT status=adc_calibration_failed\r\n");
    return false;
  }
  ready = true;
  fairy_debug::log(
      "LIGHT_GATE_INIT status=ready adc=PA6 capture=PA1 emitter=PC1 "
      "sample_us=%lu\r\n",
      static_cast<unsigned long>(fairy_board::light_gate_sample_interval_us));
  return true;
}

std::uint16_t read() {
  if (!ready || HAL_ADC_Start(&adc) != HAL_OK) {
    ++adc_failure_count;
    return 0xFFFFU;
  }
  constexpr std::uint64_t conversion_timeout_ticks = 160'000ULL;
  const std::uint64_t deadline =
      fairy_timebase::now() + conversion_timeout_ticks;
  while (__HAL_ADC_GET_FLAG(&adc, ADC_FLAG_EOC) == RESET) {
    if (fairy_timebase::now() >= deadline) {
      (void)HAL_ADC_Stop(&adc);
      ++adc_failure_count;
      return 0xFFFFU;
    }
  }
  const std::uint16_t result =
      static_cast<std::uint16_t>(HAL_ADC_GetValue(&adc));
  (void)HAL_ADC_Stop(&adc);
  latest_value = result;
  return result;
}

SelfTest self_test() {
  if (!ready) {
    fairy_debug::log("LIGHT_GATE_SELFTEST status=not_ready\r\n");
    return SelfTest{0xFFFFU,         0xFFFFU,           false,
                    clear_threshold, blocked_threshold, true};
  }

  monitor_enabled = false;
  event_pending = false;
  gate_blocked = false;

  if constexpr (fairy_board::light_gate_always_on) {
    /*
     * PC1 cannot extinguish an emitter wired directly to ground. Validate the
     * expected empty-gate baseline and use explicit hysteresis thresholds.
     */
    fairy_board::set_ir_enabled(true);
    fairy_timebase::busy_wait_us(calibration_settle_us);
    const std::uint16_t clear = averaged_reading();
    const bool valid = clear != 0xFFFFU && clear <= clear_threshold;
    fairy_board::set_ir_enabled(false);
    fairy_debug::log("LIGHT_GATE_SELFTEST mode=fixed status=%s clear=%u "
                     "clear_threshold=%u blocked_threshold=%u\r\n",
                     valid ? "passed" : "failed",
                     static_cast<unsigned int>(clear),
                     static_cast<unsigned int>(clear_threshold),
                     static_cast<unsigned int>(blocked_threshold));
    return SelfTest{0xFFFFU,           clear, valid, clear_threshold,
                    blocked_threshold, true};
  }

  fairy_board::set_ir_enabled(false);
  fairy_timebase::busy_wait_us(calibration_settle_us);
  const std::uint16_t dark = averaged_reading();
  fairy_debug::log("LIGHT_GATE_CAL emitter=off dark=%u samples=%u\r\n",
                   static_cast<unsigned int>(dark),
                   static_cast<unsigned int>(calibration_samples));
  fairy_board::set_ir_enabled(true);
  fairy_timebase::busy_wait_us(calibration_settle_us);
  const std::uint16_t clear = averaged_reading();
  fairy_debug::log("LIGHT_GATE_CAL emitter=on clear=%u samples=%u\r\n",
                   static_cast<unsigned int>(clear),
                   static_cast<unsigned int>(calibration_samples));
  const bool valid = dark != 0xFFFFU && clear != 0xFFFFU && dark > clear &&
                     static_cast<std::uint16_t>(dark - clear) >= 400U;
  const std::uint16_t span =
      valid ? static_cast<std::uint16_t>(dark - clear) : 0U;
  if (valid) {
    /*
     * A mouse or test card can interrupt only part of the optical aperture.
     * Do not require the signal to approach the fully-dark calibration value.
     * The lower release point supplies wide hysteresis around the observed
     * clear baseline.
     */
    clear_threshold = static_cast<std::uint16_t>(clear + span / 8U);
    blocked_threshold = static_cast<std::uint16_t>(clear + span / 4U);
  }
  fairy_board::set_ir_enabled(false);
  fairy_debug::log(
      "LIGHT_GATE_SELFTEST mode=controlled status=%s dark=%u clear=%u "
      "span=%u clear_threshold=%u blocked_threshold=%u\r\n",
      valid ? "passed" : "failed", static_cast<unsigned int>(dark),
      static_cast<unsigned int>(clear), static_cast<unsigned int>(span),
      static_cast<unsigned int>(clear_threshold),
      static_cast<unsigned int>(blocked_threshold));
  return SelfTest{dark,  clear, valid, clear_threshold, blocked_threshold,
                  !valid};
}

void set_emitter_enabled(bool enabled) {
  fairy_board::set_ir_enabled(enabled);
  monitor_enabled = enabled;
  gate_blocked = false;
  event_pending = false;
  const std::uint64_t now_ticks = fairy_timebase::now();
  next_sample_ticks = enabled ? now_ticks + 32'000ULL : 0ULL;
  next_status_ticks = enabled ? now_ticks + status_interval_ticks : 0ULL;
  fairy_debug::log(
      "LIGHT_GATE_CONTROL emitter=%u monitor=%u ticks_hi=%lu ticks_lo=%lu "
      "clear_threshold=%u blocked_threshold=%u\r\n",
      enabled ? 1U : 0U, enabled ? 1U : 0U, ticks_high(now_ticks),
      ticks_low(now_ticks), static_cast<unsigned int>(clear_threshold),
      static_cast<unsigned int>(blocked_threshold));
}

void service() {
  if (!ready || !monitor_enabled) {
    return;
  }

  const std::uint64_t now_ticks = fairy_timebase::now();
  if (now_ticks < next_sample_ticks) {
    return;
  }
  next_sample_ticks = now_ticks + sample_interval_ticks;

  const std::uint16_t value = read();
  if (value == 0xFFFFU) {
    fairy_debug::log("LIGHT_GATE_ADC status=read_failed failures=%lu\r\n",
                     static_cast<unsigned long>(adc_failure_count));
    return;
  }

  if (!gate_blocked && value >= blocked_threshold) {
    gate_blocked = true;
    fairy_debug::log(
        "LIGHT_GATE_STATE state=blocked adc=%u ticks_hi=%lu ticks_lo=%lu\r\n",
        static_cast<unsigned int>(value), ticks_high(now_ticks),
        ticks_low(now_ticks));
  } else if (gate_blocked && value <= clear_threshold) {
    gate_blocked = false;
    fairy_debug::log(
        "LIGHT_GATE_STATE state=clear adc=%u ticks_hi=%lu ticks_lo=%lu\r\n",
        static_cast<unsigned int>(value), ticks_high(now_ticks),
        ticks_low(now_ticks));
  }

  if (now_ticks >= next_status_ticks) {
    next_status_ticks = now_ticks + status_interval_ticks;
    fairy_debug::log(
        "LIGHT_GATE_STATUS emitter=1 state=%s adc=%u clear_threshold=%u "
        "blocked_threshold=%u\r\n",
        gate_blocked ? "blocked" : "clear", static_cast<unsigned int>(value),
        static_cast<unsigned int>(clear_threshold),
        static_cast<unsigned int>(blocked_threshold));
    fairy_debug::log("LIGHT_GATE_COUNTS captures=%lu accepted=%lu rejected=%lu "
                     "adc_events=%lu adc_failures=%lu\r\n",
                     static_cast<unsigned long>(capture_count),
                     static_cast<unsigned long>(accepted_count),
                     static_cast<unsigned long>(rejected_count),
                     static_cast<unsigned long>(adc_event_count),
                     static_cast<unsigned long>(adc_failure_count));
  }
}

void capture_edge(std::uint64_t ticks, bool overcapture) {
  ++capture_count;
  if (!ready || !monitor_enabled) {
    ++rejected_count;
    fairy_debug::log(
        "LIGHT_GATE_EDGE status=rejected reason=monitor_off ticks_hi=%lu "
        "ticks_lo=%lu captures=%lu\r\n",
        ticks_high(ticks), ticks_low(ticks),
        static_cast<unsigned long>(capture_count));
    return;
  }

  const std::uint16_t value = read();
  if (value == 0xFFFFU) {
    ++rejected_count;
    fairy_debug::log(
        "LIGHT_GATE_EDGE status=rejected reason=adc_failed ticks_hi=%lu "
        "ticks_lo=%lu adc_failures=%lu\r\n",
        ticks_high(ticks), ticks_low(ticks),
        static_cast<unsigned long>(adc_failure_count));
    return;
  }
  if (value < blocked_threshold) {
    ++rejected_count;
    fairy_debug::log(
        "LIGHT_GATE_EDGE status=rejected reason=below_threshold ticks_hi=%lu "
        "ticks_lo=%lu adc=%u blocked_threshold=%u\r\n",
        ticks_high(ticks), ticks_low(ticks), static_cast<unsigned int>(value),
        static_cast<unsigned int>(blocked_threshold));
    return;
  }
  if (event_pending) {
    ++rejected_count;
    fairy_debug::log(
        "LIGHT_GATE_EDGE status=rejected reason=event_pending ticks_hi=%lu "
        "ticks_lo=%lu adc=%u\r\n",
        ticks_high(ticks), ticks_low(ticks), static_cast<unsigned int>(value));
    return;
  }

  gate_blocked = true;
  pending_event = GateEvent{ticks, value, overcapture, true};
  event_pending = true;
  ++accepted_count;
  fairy_debug::log(
      "LIGHT_GATE_EDGE status=accepted ticks_hi=%lu ticks_lo=%lu adc=%u "
      "overcapture=%u accepted=%lu rejected=%lu\r\n",
      ticks_high(ticks), ticks_low(ticks), static_cast<unsigned int>(value),
      overcapture ? 1U : 0U, static_cast<unsigned long>(accepted_count),
      static_cast<unsigned long>(rejected_count));
}

bool pop_gate_event(GateEvent &event) {
  if (!event_pending) {
    return false;
  }
  event = pending_event;
  event_pending = false;
  return true;
}

std::uint16_t last_value() { return latest_value; }

bool monitoring() { return monitor_enabled; }

} // namespace fairy_light
