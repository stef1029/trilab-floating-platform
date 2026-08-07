#include "outputs.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <stm32g0xx_hal.h>

#include "board_profile.hpp"
#include "debug_log.hpp"
#include "fairy_shared/static_queue.hpp"
#include "light_sensor.hpp"

namespace fairy_outputs {
namespace {

inline constexpr std::uint32_t rgb_period = 31'999;
inline constexpr std::uint32_t valve_period = 2'559;
inline constexpr std::uint32_t valve_latch_pulse_us = 8'000;
inline constexpr std::uint32_t valve_startup_close_pulse_us = 10'000;
/* 64 MHz TIM6 clock / (1332 + 1) = 48012.003 samples per second. */
inline constexpr std::uint32_t audio_sample_rate = 48'012;
inline constexpr std::uint32_t maximum_audio_frequency_hz = 20'000;
/*
 * A half-buffer is about 10.7 ms. This leaves enough foreground time for one
 * maximum-size RS485 response while audio synthesis stays out of interrupt
 * context.
 */
inline constexpr std::size_t audio_buffer_samples = 1024;
inline constexpr std::size_t audio_half_buffer_samples =
    audio_buffer_samples / 2U;
inline constexpr std::size_t noise_filter_stage_count = 4;
inline constexpr std::uint32_t minimum_noise_frequency_hz = 60;
inline constexpr std::uint32_t minimum_noise_bandwidth_hz = 500;
inline constexpr std::uint32_t hard_maximum_valve_on_us = 250'000;
inline constexpr std::int32_t q15_one = 32'767;
inline constexpr std::int32_t q15_filter_limit = 16'383;
inline constexpr std::uint32_t common_timer_hz = 16'000'000;
inline constexpr std::uint32_t audio_half_deadline_ticks =
    static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(audio_half_buffer_samples) *
         common_timer_hz) /
        audio_sample_rate);
inline constexpr std::uint64_t audio_diagnostic_interval_ticks =
    common_timer_hz;
static_assert(audio_buffer_samples % 2U == 0U,
              "The circular DAC buffer must have two equal halves");
inline constexpr std::uint32_t audio_ramp_samples =
    (audio_sample_rate + 50U) / 100U;
inline constexpr std::uint32_t audio_ramp_phase_end = 1U << 30U;
inline constexpr std::uint32_t audio_ramp_phase_step =
    (audio_ramp_phase_end + audio_ramp_samples - 1U) / audio_ramp_samples;

/*
 * One quadrant of a signed Q15 sine wave. The remaining quadrants are
 * reconstructed from symmetry. This avoids floating point and std::sin() in
 * the real-time refill path on the Cortex M0+.
 */
constexpr std::array<std::int16_t, 65> sine_quarter{{
    0,     804,   1608,  2410,  3212,  4011,  4808,  5602,
    6393,  7179,  7962,  8739,  9512,  10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767,
}};

/*
 * Direct Form I biquad. Samples and state are signed Q15 values, while the
 * coefficients use Q2.14 because Butterworth feedback coefficients can be
 * almost 2. Each product and the complete accumulator fit in signed 32 bits.
 * This matters on the Cortex M0+, where 32 by 32 to 64 bit multiplication is
 * implemented in software but 16 by 16 to 32 bit multiplication is native.
 */
struct BiquadQ15 {
  std::int16_t b0{};
  std::int16_t b1{};
  std::int16_t b2{};
  std::int16_t a1{};
  std::int16_t a2{};
  std::int16_t x1{};
  std::int16_t x2{};
  std::int16_t y1{};
  std::int16_t y2{};

  std::int16_t process(std::int16_t input) {
    std::int32_t accumulator =
        static_cast<std::int32_t>(b0) * input;
    accumulator += static_cast<std::int32_t>(b1) * x1;
    accumulator += static_cast<std::int32_t>(b2) * x2;
    accumulator -= static_cast<std::int32_t>(a1) * y1;
    accumulator -= static_cast<std::int32_t>(a2) * y2;

    const std::int32_t rounded =
        accumulator >= 0 ? accumulator + (1 << 13)
                         : accumulator + ((1 << 13) - 1);
    const std::int16_t output = static_cast<std::int16_t>(
        std::clamp<std::int32_t>(rounded >> 14U, -q15_filter_limit,
                                 q15_filter_limit));
    x2 = x1;
    x1 = input;
    y2 = y1;
    y1 = output;
    return output;
  }
};

struct NoiseFilterConfiguration {
  std::array<BiquadQ15, noise_filter_stage_count> stages{};
  std::uint32_t normalization_q16{};
};

TIM_HandleTypeDef rgb_timer;
TIM_HandleTypeDef valve_timer;
TIM_HandleTypeDef audio_timer;
DAC_HandleTypeDef dac;
DMA_HandleTypeDef dac_dma;

std::array<std::uint32_t, audio_buffer_samples> audio_buffer;
fairy::StaticQueue<Event, 16> events;

ValveConfiguration valve_config;
enum class ValvePhase : std::uint8_t {
  idle,
  opening,
  open_dwell,
  closing,
};
ValvePhase valve_phase;
std::uint64_t valve_started_ticks;
std::uint64_t valve_phase_deadline_ticks;
std::uint64_t last_valve_stop_ticks;
std::uint32_t valve_open_dwell_us;

std::uint64_t rgb_stop_ticks;
std::uint64_t audio_stop_ticks;
AudioMode audio_mode;
std::uint16_t audio_amplitude;
std::uint32_t tone_phase;
std::uint32_t tone_phase_step;
std::array<BiquadQ15, noise_filter_stage_count> noise_filter;
std::uint32_t noise_normalization_q16;
std::uint32_t random_state = 0x78D29A4BU;
std::uint32_t audio_ramp_phase;
volatile bool audio_engine_running;
volatile bool audio_stopping;
volatile bool audio_fault;
volatile std::int32_t audio_gain_q15;
volatile std::uint32_t audio_dma_irq_count;
volatile std::uint32_t audio_dma_half_count;
volatile std::uint32_t audio_dma_full_count;
volatile std::uint32_t audio_dma_late_fill_count;
volatile std::uint32_t audio_dma_callback_gap_count;
volatile std::uint32_t audio_dma_max_fill_ticks;
volatile std::uint32_t audio_dma_max_callback_gap_ticks;
volatile std::uint32_t audio_dma_last_callback_tick;
volatile std::uint8_t audio_refill_pending;
volatile std::uint32_t audio_first_refill_tick;
volatile std::uint32_t audio_second_refill_tick;
std::uint64_t audio_last_diagnostic_ticks;
bool audio_active;
bool ir_enabled;

std::uint32_t duty_to_compare(std::uint16_t per_mille) {
  return static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(per_mille) * (valve_period + 1U)) / 1000U);
}

void set_valve_outputs(std::uint16_t in1_per_mille,
                       std::uint16_t in2_per_mille) {
  __HAL_TIM_SET_COMPARE(&valve_timer, TIM_CHANNEL_1,
                        duty_to_compare(in1_per_mille));
  __HAL_TIM_SET_COMPARE(&valve_timer, TIM_CHANNEL_2,
                        duty_to_compare(in2_per_mille));
}

void valve_idle() { set_valve_outputs(0, 0); }

void valve_open_pulse() { set_valve_outputs(1000, 0); }

void valve_close_pulse() { set_valve_outputs(0, 1000); }

void set_rgb_channel(std::uint32_t channel, std::uint8_t value) {
  const std::uint32_t compare =
      (static_cast<std::uint32_t>(value) * (rgb_period + 1U)) / 255U;
  __HAL_TIM_SET_COMPARE(&rgb_timer, channel, compare);
}

void queue_event(EventKind kind, std::uint64_t ticks, std::uint32_t value = 0,
                 std::uint32_t duration_us = 0) {
  (void)events.push(Event{kind, ticks, value, duration_us});
}

std::int32_t sine_q15(std::uint32_t phase) {
  const std::uint32_t quadrant = phase >> 30U;
  const std::uint32_t offset = (phase >> 24U) & 0x3FU;
  switch (quadrant) {
    case 0:
      return sine_quarter[offset];
    case 1:
      return sine_quarter[64U - offset];
    case 2:
      return -sine_quarter[offset];
    default:
      return -sine_quarter[64U - offset];
  }
}

std::int32_t raised_cosine_gain_q15(std::uint32_t phase) {
  const std::int32_t sine = sine_q15(phase);
  return (sine * sine) >> 15U;
}

std::int16_t coefficient_q14(double value) {
  constexpr double q14_scale = 16'384.0;
  const double scaled = value * q14_scale;
  if (scaled >= static_cast<double>(std::numeric_limits<std::int16_t>::max())) {
    return std::numeric_limits<std::int16_t>::max();
  }
  if (scaled <= static_cast<double>(std::numeric_limits<std::int16_t>::min())) {
    return std::numeric_limits<std::int16_t>::min();
  }
  return static_cast<std::int16_t>(std::llround(scaled));
}

enum class BiquadKind {
  low_pass,
  high_pass,
};

BiquadQ15 butterworth_section(BiquadKind kind, double sine, double cosine,
                              double quality_factor) {
  const double alpha = sine / (2.0 * quality_factor);
  const double a0 = 1.0 + alpha;

  double b0{};
  double b1{};
  double b2{};
  if (kind == BiquadKind::low_pass) {
    b0 = (1.0 - cosine) * 0.5;
    b1 = 1.0 - cosine;
    b2 = b0;
  } else {
    b0 = (1.0 + cosine) * 0.5;
    b1 = -(1.0 + cosine);
    b2 = b0;
  }

  BiquadQ15 section{};
  section.b0 = coefficient_q14(b0 / a0);
  section.b1 = coefficient_q14(b1 / a0);
  section.b2 = coefficient_q14(b2 / a0);
  section.a1 = coefficient_q14((-2.0 * cosine) / a0);
  section.a2 = coefficient_q14((1.0 - alpha) / a0);
  return section;
}

NoiseFilterConfiguration make_noise_filter(std::uint32_t low_hz,
                                           std::uint32_t high_hz) {
  /* Q values for the two pole pairs of a fourth order Butterworth filter. */
  constexpr double pi = 3.14159265358979323846;
  constexpr double butterworth_q0 = 0.541196100146197;
  constexpr double butterworth_q1 = 1.306562964876377;

  const double low_omega =
      2.0 * pi * static_cast<double>(low_hz) /
      static_cast<double>(audio_sample_rate);
  const double high_omega =
      2.0 * pi * static_cast<double>(high_hz) /
      static_cast<double>(audio_sample_rate);
  const double low_sine = std::sin(low_omega);
  const double low_cosine = std::cos(low_omega);
  const double high_sine = std::sin(high_omega);
  const double high_cosine = std::cos(high_omega);

  NoiseFilterConfiguration configuration{};
  /*
   * Put the lower Q sections first. They remove out of band energy before
   * the more resonant sections and preserve fixed point headroom.
  */
  configuration.stages[0] =
      butterworth_section(BiquadKind::high_pass, low_sine, low_cosine,
                          butterworth_q0);
  configuration.stages[1] =
      butterworth_section(BiquadKind::low_pass, high_sine, high_cosine,
                          butterworth_q0);
  configuration.stages[2] =
      butterworth_section(BiquadKind::high_pass, low_sine, low_cosine,
                          butterworth_q1);
  configuration.stages[3] =
      butterworth_section(BiquadKind::low_pass, high_sine, high_cosine,
                          butterworth_q1);

  /*
   * A uniform random source has RMS 1/sqrt(3). The source is divided by four
   * before the biquads for headroom. Scale an ideal band of the requested
   * width to approximately -12 dBFS RMS. This keeps different bandwidths at
   * similar levels without a time varying AGC.
   */
  const double occupied_fraction =
      (2.0 * static_cast<double>(high_hz - low_hz)) /
      static_cast<double>(audio_sample_rate);
  const double normalization =
      std::sqrt(3.0) / std::sqrt(occupied_fraction);
  constexpr double maximum_normalization = 1024.0;
  const double limited = std::min(normalization, maximum_normalization);
  configuration.normalization_q16 = static_cast<std::uint32_t>(
      std::llround(limited * 65'536.0));
  return configuration;
}

std::int32_t next_signal_q15() {
  if (audio_mode == AudioMode::tone) {
    const std::int32_t signal = sine_q15(tone_phase);
    tone_phase += tone_phase_step;
    return signal;
  }

  if (audio_mode == AudioMode::white_noise_band) {
    random_state ^= random_state << 13U;
    random_state ^= random_state >> 17U;
    random_state ^= random_state << 5U;
    /* Four times headroom prevents resonant intermediate stages clipping. */
    const std::int32_t random_q15 =
        static_cast<std::int32_t>(random_state >> 16U) - 32'768;
    std::int16_t filtered_q15 = static_cast<std::int16_t>(
        random_q15 / 4);
    for (auto& stage : noise_filter) {
      filtered_q15 = stage.process(filtered_q15);
    }

    const std::int64_t normalized_q15 =
        (static_cast<std::int64_t>(filtered_q15) *
         noise_normalization_q16) >>
        16U;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        normalized_q15, -32'768, 32'767));
  }

  return 0;
}

void fill_audio(std::size_t offset, std::size_t count) {
  for (std::size_t index = 0; index < count; ++index) {
    if (audio_stopping) {
      audio_ramp_phase =
          audio_ramp_phase > audio_ramp_phase_step
              ? audio_ramp_phase - audio_ramp_phase_step
              : 0U;
    } else {
      audio_ramp_phase =
          std::min(audio_ramp_phase_end,
                   audio_ramp_phase + audio_ramp_phase_step);
    }
    audio_gain_q15 = raised_cosine_gain_q15(audio_ramp_phase);

    const std::int32_t signal_q15 = next_signal_q15();
    const std::int32_t amplitude_scaled =
        (signal_q15 * static_cast<std::int32_t>(audio_amplitude)) >> 15U;
    const std::int32_t scaled =
        (amplitude_scaled * audio_gain_q15) >> 15U;
    audio_buffer[offset + index] = static_cast<std::uint32_t>(
        std::clamp<std::int32_t>(2048 + scaled, 0, 4095));
  }
}

void update_maximum(volatile std::uint32_t& maximum, std::uint32_t value) {
  if (value > maximum) {
    maximum = value;
  }
}

void request_audio_refill(bool first_half) {
  const std::uint32_t callback_tick = TIM2->CNT;
  const std::uint32_t previous_tick = audio_dma_last_callback_tick;
  audio_dma_last_callback_tick = callback_tick;

  if (previous_tick != 0U) {
    const std::uint32_t gap = callback_tick - previous_tick;
    update_maximum(audio_dma_max_callback_gap_ticks, gap);
    if (gap > audio_half_deadline_ticks + audio_half_deadline_ticks / 2U) {
      ++audio_dma_callback_gap_count;
    }
  }

  if (first_half) {
    ++audio_dma_half_count;
    audio_first_refill_tick = callback_tick;
  } else {
    ++audio_dma_full_count;
    audio_second_refill_tick = callback_tick;
  }

  const std::uint8_t pending_bit = first_half ? 0x01U : 0x02U;
  if ((audio_refill_pending & pending_bit) != 0U) {
    /* The DMA has reached this half again before foreground serviced it. */
    ++audio_dma_late_fill_count;
    audio_fault = true;
  }
  audio_refill_pending |= pending_bit;
}

void service_audio_refills() {
  const std::uint32_t primask = __get_PRIMASK();
  __disable_irq();
  const std::uint8_t pending = audio_refill_pending;
  const std::uint32_t first_requested = audio_first_refill_tick;
  const std::uint32_t second_requested = audio_second_refill_tick;
  audio_refill_pending = 0;
  if (primask == 0U) {
    __enable_irq();
  }

  if (!audio_engine_running || pending == 0U) {
    return;
  }

  const auto service_half = [](std::size_t offset,
                               std::uint32_t requested_tick) {
    if (TIM2->CNT - requested_tick >= audio_half_deadline_ticks) {
      ++audio_dma_late_fill_count;
      audio_fault = true;
      return;
    }
    const std::uint32_t fill_started = TIM2->CNT;
    fill_audio(offset, audio_half_buffer_samples);
    update_maximum(audio_dma_max_fill_ticks, TIM2->CNT - fill_started);
    if (TIM2->CNT - requested_tick >= audio_half_deadline_ticks) {
      ++audio_dma_late_fill_count;
      audio_fault = true;
    }
  };

  if ((pending & 0x01U) != 0U) {
    service_half(0, first_requested);
  }
  if ((pending & 0x02U) != 0U && !audio_fault) {
    service_half(audio_half_buffer_samples, second_requested);
  }
}

void reset_audio_diagnostics() {
  audio_dma_irq_count = 0;
  audio_dma_half_count = 0;
  audio_dma_full_count = 0;
  audio_dma_late_fill_count = 0;
  audio_dma_callback_gap_count = 0;
  audio_dma_max_fill_ticks = 0;
  audio_dma_max_callback_gap_ticks = 0;
  audio_dma_last_callback_tick = 0;
  audio_refill_pending = 0;
  audio_first_refill_tick = 0;
  audio_second_refill_tick = 0;
}

void initialize_rgb() {
  __HAL_RCC_TIM3_CLK_ENABLE();
  rgb_timer.Instance = TIM3;
  rgb_timer.Init.Prescaler = 0;
  rgb_timer.Init.CounterMode = TIM_COUNTERMODE_UP;
  rgb_timer.Init.Period = rgb_period;
  rgb_timer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  rgb_timer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_PWM_Init(&rgb_timer) != HAL_OK) {
    Error_Handler();
  }
  TIM_OC_InitTypeDef pwm{};
  pwm.OCMode = TIM_OCMODE_PWM1;
  pwm.Pulse = 0;
  pwm.OCPolarity = TIM_OCPOLARITY_HIGH;
  pwm.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&rgb_timer, &pwm, TIM_CHANNEL_1) != HAL_OK ||
      HAL_TIM_PWM_ConfigChannel(&rgb_timer, &pwm, TIM_CHANNEL_2) != HAL_OK ||
      HAL_TIM_PWM_ConfigChannel(&rgb_timer, &pwm, TIM_CHANNEL_3) != HAL_OK ||
      HAL_TIM_PWM_Start(&rgb_timer, TIM_CHANNEL_1) != HAL_OK ||
      HAL_TIM_PWM_Start(&rgb_timer, TIM_CHANNEL_2) != HAL_OK ||
      HAL_TIM_PWM_Start(&rgb_timer, TIM_CHANNEL_3) != HAL_OK) {
    Error_Handler();
  }
}

void initialize_valve() {
  __HAL_RCC_TIM1_CLK_ENABLE();
  valve_timer.Instance = TIM1;
  valve_timer.Init.Prescaler = 0;
  valve_timer.Init.CounterMode = TIM_COUNTERMODE_UP;
  valve_timer.Init.Period = valve_period;
  valve_timer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  valve_timer.Init.RepetitionCounter = 0;
  valve_timer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_PWM_Init(&valve_timer) != HAL_OK) {
    Error_Handler();
  }
  TIM_OC_InitTypeDef pwm{};
  pwm.OCMode = TIM_OCMODE_PWM1;
  pwm.Pulse = 0;
  pwm.OCPolarity = TIM_OCPOLARITY_HIGH;
  pwm.OCNPolarity = TIM_OCNPOLARITY_HIGH;
  pwm.OCFastMode = TIM_OCFAST_DISABLE;
  pwm.OCIdleState = TIM_OCIDLESTATE_RESET;
  pwm.OCNIdleState = TIM_OCIDLESTATE_RESET;
  if (HAL_TIM_PWM_ConfigChannel(&valve_timer, &pwm, TIM_CHANNEL_1) != HAL_OK ||
      HAL_TIM_PWM_ConfigChannel(&valve_timer, &pwm, TIM_CHANNEL_2) != HAL_OK ||
      HAL_TIM_PWM_Start(&valve_timer, TIM_CHANNEL_1) != HAL_OK ||
      HAL_TIM_PWM_Start(&valve_timer, TIM_CHANNEL_2) != HAL_OK) {
    Error_Handler();
  }
}

void initialize_audio() {
  __HAL_RCC_DAC1_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();
  __HAL_RCC_TIM6_CLK_ENABLE();

  dac.Instance = DAC1;
  if (HAL_DAC_Init(&dac) != HAL_OK) {
    Error_Handler();
  }
  DAC_ChannelConfTypeDef channel{};
  channel.DAC_Trigger = DAC_TRIGGER_T6_TRGO;
  channel.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
  channel.DAC_ConnectOnChipPeripheral = DAC_CHIPCONNECT_DISABLE;
  channel.DAC_UserTrimming = DAC_TRIMMING_FACTORY;
  if (HAL_DAC_ConfigChannel(&dac, &channel, DAC_CHANNEL_1) != HAL_OK) {
    Error_Handler();
  }

  dac_dma.Instance = DMA1_Channel1;
  dac_dma.Init.Request = DMA_REQUEST_DAC1_CHANNEL1;
  dac_dma.Init.Direction = DMA_MEMORY_TO_PERIPH;
  dac_dma.Init.PeriphInc = DMA_PINC_DISABLE;
  dac_dma.Init.MemInc = DMA_MINC_ENABLE;
  dac_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
  dac_dma.Init.MemDataAlignment = DMA_MDATAALIGN_WORD;
  dac_dma.Init.Mode = DMA_CIRCULAR;
  dac_dma.Init.Priority = DMA_PRIORITY_MEDIUM;
  if (HAL_DMA_Init(&dac_dma) != HAL_OK) {
    Error_Handler();
  }
  __HAL_LINKDMA(&dac, DMA_Handle1, dac_dma);
  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);

  audio_timer.Instance = TIM6;
  audio_timer.Init.Prescaler = 0;
  audio_timer.Init.CounterMode = TIM_COUNTERMODE_UP;
  audio_timer.Init.Period = 1'332;
  audio_timer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&audio_timer) != HAL_OK) {
    Error_Handler();
  }
  TIM_MasterConfigTypeDef master{};
  master.MasterOutputTrigger = TIM_TRGO_UPDATE;
  master.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&audio_timer, &master) != HAL_OK) {
    Error_Handler();
  }

  audio_buffer.fill(2048);
  audio_engine_running = false;
  audio_active = false;
  audio_stopping = false;
  audio_fault = false;
  audio_gain_q15 = 0;
  audio_ramp_phase = 0;
  noise_normalization_q16 = 65'536U;
  noise_filter.fill(BiquadQ15{});
  reset_audio_diagnostics();
  audio_last_diagnostic_ticks = 0;
  if (HAL_DAC_Start(&dac, DAC_CHANNEL_1) != HAL_OK ||
      HAL_DAC_SetValue(&dac, DAC_CHANNEL_1, DAC_ALIGN_12B_R, 2048U) !=
          HAL_OK) {
    Error_Handler();
  }
}

bool start_audio_engine() {
  if (audio_engine_running) {
    return true;
  }

  audio_fault = false;
  reset_audio_diagnostics();
  fill_audio(0, audio_buffer.size());
  if (HAL_DAC_Start_DMA(&dac, DAC_CHANNEL_1, audio_buffer.data(),
                        audio_buffer.size(), DAC_ALIGN_12B_R) != HAL_OK) {
    return false;
  }

  audio_engine_running = true;
  if (HAL_TIM_Base_Start(&audio_timer) != HAL_OK) {
    audio_engine_running = false;
    (void)HAL_DAC_Stop_DMA(&dac, DAC_CHANNEL_1);
    (void)HAL_DAC_Start(&dac, DAC_CHANNEL_1);
    (void)HAL_DAC_SetValue(&dac, DAC_CHANNEL_1, DAC_ALIGN_12B_R, 2048U);
    return false;
  }
  return true;
}

void stop_audio_engine() {
  if (audio_engine_running) {
    audio_engine_running = false;
    (void)HAL_TIM_Base_Stop(&audio_timer);
    (void)HAL_DAC_Stop_DMA(&dac, DAC_CHANNEL_1);
  }
  const std::uint32_t primask = __get_PRIMASK();
  __disable_irq();
  audio_refill_pending = 0;
  if (primask == 0U) {
    __enable_irq();
  }
  (void)HAL_DAC_Start(&dac, DAC_CHANNEL_1);
  (void)HAL_DAC_SetValue(&dac, DAC_CHANNEL_1, DAC_ALIGN_12B_R, 2048U);
}

void finish_audio(std::uint64_t now_ticks) {
  const bool was_active = audio_active;
  audio_active = false;
  audio_stopping = false;
  audio_fault = false;
  audio_stop_ticks = 0;
  audio_amplitude = 0;
  audio_mode = AudioMode::off;
  audio_gain_q15 = 0;
  audio_ramp_phase = 0;
  stop_audio_engine();
  fairy_board::set_amplifier_enabled(false);
  if (was_active) {
    queue_event(EventKind::audio_stopped, now_ticks);
  }
}

void begin_audio_stop() {
  if (audio_active) {
    audio_stopping = true;
    audio_stop_ticks = 0;
  }
}

void finish_valve_cycle(std::uint64_t now_ticks) {
  valve_idle();
  valve_phase = ValvePhase::idle;
  valve_phase_deadline_ticks = 0;
  last_valve_stop_ticks = now_ticks;
  queue_event(EventKind::valve_stopped, now_ticks, 0,
              static_cast<std::uint32_t>((now_ticks - valve_started_ticks) /
                                         16U));
}

void begin_valve_close(std::uint64_t now_ticks) {
  valve_close_pulse();
  valve_phase = ValvePhase::closing;
  valve_phase_deadline_ticks =
      now_ticks + static_cast<std::uint64_t>(valve_latch_pulse_us) * 16U;
}

}  // namespace

void initialize() {
  events.clear();
  valve_phase = ValvePhase::idle;
  valve_phase_deadline_ticks = 0;
  last_valve_stop_ticks = 0;
  valve_open_dwell_us = 0;
  if constexpr (fairy_board::prototype) {
    /*
     * The wired prototype has a fixed 5 V valve supply. Arm the bounded
     * latching defaults even if Adelie has not sent CONFIGURE_VALVE yet.
     */
    valve_config = ValveConfiguration{};
    valve_config.valid = true;
  }
  fairy_board::set_amplifier_enabled(false);
  fairy_board::set_ir_enabled(false);
  initialize_rgb();
  initialize_valve();
  initialize_audio();
  all_safe();

  /* Match the legacy controller: establish the known closed latch at boot. */
  valve_close_pulse();
  HAL_Delay((valve_startup_close_pulse_us + 999U) / 1000U);
  valve_idle();
  events.clear();
}

void service(std::uint64_t now_ticks) {
  service_audio_refills();
  if (audio_active &&
      now_ticks - audio_last_diagnostic_ticks >=
          audio_diagnostic_interval_ticks) {
    const std::uint32_t primask = __get_PRIMASK();
    __disable_irq();
    const std::uint32_t irq_count = audio_dma_irq_count;
    const std::uint32_t half_count = audio_dma_half_count;
    const std::uint32_t full_count = audio_dma_full_count;
    const std::uint32_t late_count = audio_dma_late_fill_count;
    const std::uint32_t gap_count = audio_dma_callback_gap_count;
    const std::uint32_t max_fill_ticks = audio_dma_max_fill_ticks;
    const std::uint32_t max_gap_ticks = audio_dma_max_callback_gap_ticks;
    if (primask == 0U) {
      __enable_irq();
    }
    fairy_debug::log(
        "AUDIO_DMA irq=%lu half=%lu full=%lu late=%lu gaps=%lu "
        "max_fill_us=%lu max_gap_us=%lu deadline_us=%lu\r\n",
        static_cast<unsigned long>(irq_count),
        static_cast<unsigned long>(half_count),
        static_cast<unsigned long>(full_count),
        static_cast<unsigned long>(late_count),
        static_cast<unsigned long>(gap_count),
        static_cast<unsigned long>(max_fill_ticks / 16U),
        static_cast<unsigned long>(max_gap_ticks / 16U),
        static_cast<unsigned long>(audio_half_deadline_ticks / 16U));
    audio_last_diagnostic_ticks = now_ticks;
  }
  if (rgb_stop_ticks != 0U && now_ticks >= rgb_stop_ticks) {
    set_rgb(0, 0, 0, 0, now_ticks);
  }
  if (audio_active && audio_stop_ticks != 0U &&
      now_ticks >= audio_stop_ticks) {
    begin_audio_stop();
  }
  if (audio_active && audio_stopping && audio_gain_q15 == 0) {
    finish_audio(now_ticks);
  }
  if (audio_active && audio_fault) {
    finish_audio(now_ticks);
  }
  if (valve_phase != ValvePhase::idle &&
      now_ticks >= valve_phase_deadline_ticks) {
    switch (valve_phase) {
      case ValvePhase::opening:
        valve_idle();
        valve_phase = ValvePhase::open_dwell;
        valve_phase_deadline_ticks =
            now_ticks + static_cast<std::uint64_t>(valve_open_dwell_us) * 16U;
        break;

      case ValvePhase::open_dwell:
        begin_valve_close(now_ticks);
        break;

      case ValvePhase::closing:
        finish_valve_cycle(now_ticks);
        break;

      case ValvePhase::idle:
        break;
    }
  }
}

void all_safe(std::uint64_t now_ticks) {
  set_rgb(0, 0, 0, 0, now_ticks);
  finish_audio(now_ticks);
  if (valve_phase == ValvePhase::opening ||
      valve_phase == ValvePhase::open_dwell) {
    /* A stop, timeout, or session change must actively re-latch closed. */
    begin_valve_close(now_ticks);
  } else if (valve_phase == ValvePhase::idle) {
    valve_idle();
  }
  if (ir_enabled) {
    set_ir(false, now_ticks);
  } else {
    fairy_light::set_emitter_enabled(false);
  }
}

void set_rgb(std::uint8_t red, std::uint8_t green, std::uint8_t blue,
             std::uint32_t duration_ms, std::uint64_t now_ticks) {
  set_rgb_channel(TIM_CHANNEL_1, red);
  set_rgb_channel(TIM_CHANNEL_2, green);
  set_rgb_channel(TIM_CHANNEL_3, blue);
  rgb_stop_ticks =
      duration_ms == 0U ? 0U : now_ticks + duration_ms * 16'000ULL;
  const std::uint32_t packed =
      static_cast<std::uint32_t>(red) |
      (static_cast<std::uint32_t>(green) << 8U) |
      (static_cast<std::uint32_t>(blue) << 16U);
  queue_event(EventKind::rgb, now_ticks, packed, duration_ms * 1000U);
}

void set_ir(bool enabled, std::uint64_t now_ticks) {
  ir_enabled = enabled;
  fairy_light::set_emitter_enabled(enabled);
  queue_event(EventKind::ir, now_ticks, enabled ? 1U : 0U);
}

fairy::protocol::Status set_audio(AudioMode mode, std::uint32_t frequency_hz,
                                  std::uint32_t low_hz,
                                  std::uint32_t high_hz,
                                  std::uint16_t amplitude,
                                  std::uint32_t duration_ms,
                                  std::uint64_t now_ticks) {
  if (mode == AudioMode::off) {
    begin_audio_stop();
    return fairy::protocol::Status::ok;
  }
  if ((mode != AudioMode::tone && mode != AudioMode::white_noise_band) ||
      amplitude > 2047U ||
      (mode == AudioMode::tone &&
       (frequency_hz < 20U ||
        frequency_hz > maximum_audio_frequency_hz)) ||
      (mode == AudioMode::white_noise_band &&
       (low_hz < minimum_noise_frequency_hz || high_hz <= low_hz ||
        high_hz > maximum_audio_frequency_hz ||
        high_hz - low_hz < minimum_noise_bandwidth_hz))) {
    return fairy::protocol::Status::invalid_parameter;
  }

  NoiseFilterConfiguration new_noise_filter{};
  if (mode == AudioMode::white_noise_band) {
    new_noise_filter = make_noise_filter(low_hz, high_hz);
  }

  const std::uint32_t primask = __get_PRIMASK();
  __disable_irq();
  audio_amplitude = amplitude;
  audio_mode = mode;
  audio_active = true;
  audio_stopping = false;
  audio_gain_q15 = 0;
  audio_ramp_phase = 0;
  reset_audio_diagnostics();
  audio_last_diagnostic_ticks = now_ticks;
  audio_stop_ticks =
      duration_ms == 0U ? 0U : now_ticks + duration_ms * 16'000ULL;
  tone_phase = 0;
  tone_phase_step =
      mode == AudioMode::tone
          ? static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(frequency_hz) << 32U) /
                audio_sample_rate)
          : 0U;
  if (mode == AudioMode::white_noise_band) {
    noise_filter = new_noise_filter.stages;
    noise_normalization_q16 = new_noise_filter.normalization_q16;
  }
  if (primask == 0U) {
    __enable_irq();
  }

  if (!start_audio_engine()) {
    finish_audio(now_ticks);
    return fairy::protocol::Status::internal_error;
  }

  fairy_board::set_amplifier_enabled(true);
  queue_event(EventKind::audio_started, now_ticks,
              static_cast<std::uint32_t>(mode), duration_ms * 1000U);
  return fairy::protocol::Status::ok;
}

fairy::protocol::Status configure_valve(
    const ValveConfiguration& configuration) {
  if (configuration.vload_mv != 5000U ||
      configuration.spike_duration_us < 1000U ||
      configuration.spike_duration_us > 20'000U ||
      configuration.spike_duty_per_mille > 1000U ||
      configuration.hold_duty_per_mille > 1000U ||
      configuration.maximum_on_us == 0U ||
      configuration.maximum_on_us > hard_maximum_valve_on_us ||
      configuration.minimum_interval_us < 50'000U) {
    return fairy::protocol::Status::invalid_parameter;
  }
  valve_config = configuration;
  /*
   * Adelie v2 still sends the former spike and hold fields. This board uses
   * them only for wire compatibility; the two-wire latching valve always
   * receives a full-duty 8 ms transition pulse and no holding current.
   */
  valve_config.spike_duration_us = valve_latch_pulse_us;
  valve_config.spike_duty_per_mille = 1000;
  valve_config.hold_duty_per_mille = 0;
  valve_config.valid = true;
  return fairy::protocol::Status::ok;
}

fairy::protocol::Status actuate_valve(std::uint32_t duration_ms,
                                     std::uint64_t now_ticks) {
  if (!valve_config.valid) {
    return fairy::protocol::Status::safety_lock;
  }
  const std::uint32_t duration_us = duration_ms * 1000U;
  if (valve_phase != ValvePhase::idle || duration_us == 0U ||
      duration_us > valve_config.maximum_on_us) {
    return fairy::protocol::Status::invalid_parameter;
  }
  if (last_valve_stop_ticks != 0U &&
      now_ticks - last_valve_stop_ticks <
          static_cast<std::uint64_t>(valve_config.minimum_interval_us) * 16U) {
    return fairy::protocol::Status::busy;
  }
  valve_started_ticks = now_ticks;
  valve_open_dwell_us = duration_us;
  valve_phase = ValvePhase::opening;
  valve_phase_deadline_ticks =
      now_ticks + static_cast<std::uint64_t>(valve_latch_pulse_us) * 16U;
  valve_open_pulse();
  queue_event(EventKind::valve_started, now_ticks, 1, duration_us);
  return fairy::protocol::Status::ok;
}

bool pop_event(Event& event) { return events.pop(event); }

const ValveConfiguration& valve_configuration() { return valve_config; }

extern "C" void DMA1_Channel1_IRQHandler() {
  ++audio_dma_irq_count;
  HAL_DMA_IRQHandler(&dac_dma);
}

extern "C" void HAL_DAC_ConvHalfCpltCallbackCh1(DAC_HandleTypeDef* handle) {
  if (handle == &dac && audio_engine_running) {
    request_audio_refill(true);
  }
}

extern "C" void HAL_DAC_ConvCpltCallbackCh1(DAC_HandleTypeDef* handle) {
  if (handle == &dac && audio_engine_running) {
    request_audio_refill(false);
  }
}

extern "C" void HAL_DAC_ErrorCallbackCh1(DAC_HandleTypeDef* handle) {
  if (handle == &dac) {
    audio_fault = true;
  }
}

extern "C" void HAL_DAC_DMAUnderrunCallbackCh1(DAC_HandleTypeDef* handle) {
  if (handle == &dac) {
    audio_fault = true;
  }
}

}  // namespace fairy_outputs
