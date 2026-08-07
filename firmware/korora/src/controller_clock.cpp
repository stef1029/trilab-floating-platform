#include "controller_clock.hpp"

#include <cerrno>

#include <hal/nrf_rtc.h>
#include <nrfx_rtc.h>

#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>

#include "debug_log.hpp"
#include "timebase.hpp"

namespace korora_controller_clock {
namespace {

inline constexpr std::uint64_t rtc_tick_femtoseconds = 30'517'578'125ULL;
inline constexpr std::uint64_t rtc_overflow_us = 512'000'000ULL;

nrfx_rtc_t application_rtc = NRFX_RTC_INSTANCE(NRF_RTC_INST_GET(2));
volatile std::uint32_t rtc_overflows;
std::int32_t controller_offset;
fairy::time::AffineClockModel<16> bridge;
k_mutex bridge_mutex;
K_SEM_DEFINE(start_sem, 0, 1);

std::uint64_t rtc_to_us(std::uint32_t ticks) {
  return static_cast<std::uint64_t>(ticks) * rtc_tick_femtoseconds /
         1'000'000'000ULL;
}

void rtc_handler(nrf_rtc_event_t event, void *) {
  if (event == NRF_RTC_EVENT_OVERFLOW) {
    ++rtc_overflows;
  }
}

std::int32_t measure_offset() {
  std::uint32_t difference = (nrf_rtc_counter_get(NRF_RTC0) -
                              nrf_rtc_counter_get(application_rtc.p_reg)) &
                             0x00FF'FFFFU;
  if ((difference & 0x0080'0000U) != 0U) {
    difference |= 0xFF00'0000U;
  }
  return static_cast<std::int32_t>(difference);
}

std::uint64_t controller_us() {
  std::uint32_t overflows;
  std::uint32_t ticks;
  do {
    overflows = rtc_overflows;
    barrier_isync_fence_full();
    ticks = nrf_rtc_counter_get(application_rtc.p_reg);
    barrier_isync_fence_full();
  } while (overflows != rtc_overflows);
  const std::int64_t base =
      static_cast<std::int64_t>(rtc_to_us(ticks)) +
      static_cast<std::int64_t>(overflows * rtc_overflow_us);
  const std::int64_t offset_us =
      static_cast<std::int64_t>(controller_offset) *
      static_cast<std::int64_t>(rtc_tick_femtoseconds) / 1'000'000'000LL;
  const std::int64_t result = base + offset_us;
  return result < 0 ? 0U : static_cast<std::uint64_t>(result);
}

void sample_thread(void *, void *, void *) {
  k_sem_take(&start_sem, K_FOREVER);
  while (true) {
    k_sleep(K_MSEC(250));
    const std::uint64_t before = korora_time::now();
    const std::uint64_t controller_ticks = controller_us() * 16ULL;
    const std::uint64_t after = korora_time::now();
    if (after < before || after - before > 32'000ULL) {
      continue;
    }
    const std::uint64_t midpoint = before + (after - before) / 2ULL;
    k_mutex_lock(&bridge_mutex, K_FOREVER);
    (void)bridge.add(controller_ticks, midpoint);
    k_mutex_unlock(&bridge_mutex);
  }
}

K_THREAD_DEFINE(sample_thread_id, 1536, sample_thread, nullptr, nullptr,
                nullptr, 8, 0, 0);

} // namespace

int initialize() {
  k_mutex_init(&bridge_mutex);
  bridge.set_admission(3200.0, 6.0, 3);
  nrfx_rtc_config_t config = NRFX_RTC_DEFAULT_CONFIG;
  int error = nrfx_rtc_init(&application_rtc, &config, rtc_handler);
  if (error != 0) {
    return error;
  }
  IRQ_CONNECT(NRFX_IRQ_NUMBER_GET(NRF_RTC_INST_GET(2)), IRQ_PRIO_LOWEST,
              nrfx_rtc_irq_handler, &application_rtc, 0);
  nrfx_rtc_overflow_enable(&application_rtc, true);
  nrfx_rtc_tick_enable(&application_rtc, false);
  nrfx_rtc_enable(&application_rtc);

  for (unsigned int attempt = 0; attempt < 10; ++attempt) {
    const std::int32_t first = measure_offset();
    k_busy_wait(15);
    const std::int32_t second = measure_offset();
    if (first == second) {
      controller_offset = first;
      k_sem_give(&start_sem);
      return 0;
    }
  }
  return -EIO;
}

bool to_korora(std::uint64_t controller_ticks, std::uint64_t &korora_ticks) {
  const std::uint64_t raw_us = controller_ticks / 16ULL;
  const std::uint64_t near_us = controller_us();
  std::uint64_t extended_us =
      (near_us & ~0xFFFF'FFFFULL) | (raw_us & 0xFFFF'FFFFULL);
  if (extended_us + 0x8000'0000ULL < near_us) {
    extended_us += 0x1'0000'0000ULL;
  } else if (extended_us > near_us + 0x8000'0000ULL &&
             extended_us >= 0x1'0000'0000ULL) {
    extended_us -= 0x1'0000'0000ULL;
  }
  k_mutex_lock(&bridge_mutex, K_FOREVER);
  const bool result = bridge.predict(extended_us * 16ULL, korora_ticks);
  k_mutex_unlock(&bridge_mutex);
  return result;
}

fairy::time::ClockQuality quality() {
  k_mutex_lock(&bridge_mutex, K_FOREVER);
  const auto result = bridge.quality();
  k_mutex_unlock(&bridge_mutex);
  return result;
}

} // namespace korora_controller_clock
