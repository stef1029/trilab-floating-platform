#include "timebase.hpp"

#include <cerrno>

#include <gpiote_nrfx.h>
#include <hal/nrf_clock.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_timer.h>
#include <helpers/nrfx_gppi.h>
#include <nrfx_gpiote.h>
#include <nrfx_timer.h>

#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/onoff.h>

#include <zephyr/drivers/clock_control/nrf_clock_control.h>

#include "debug_log.hpp"
#include "fairy_shared/system_config.hpp"

namespace korora_time {
namespace {

#define USER_NODE DT_PATH(zephyr_user)
#define TIMER_NODE DT_NODELABEL(timer2)
#define SYNC_PIN NRF_DT_GPIOS_TO_PSEL(USER_NODE, sync_gpios)
#define GPIOTE_NODE NRF_DT_GPIOTE_NODE(USER_NODE, sync_gpios)

#if DT_NODE_HAS_PROP(USER_NODE, sync_dir_gpios)
#define KORORA_HAS_SYNC_DIR 1
#define SYNC_DIR_PIN NRF_DT_GPIOS_TO_PSEL(USER_NODE, sync_dir_gpios)
#else
#define KORORA_HAS_SYNC_DIR 0
#endif

inline constexpr std::uint32_t period_ticks =
    fairy::config::common_timer_hz / fairy::config::sync_rate_hz;
inline constexpr std::uint32_t pulse_width_ticks = 1600;

nrfx_timer_t timer = NRFX_TIMER_INSTANCE(NRF_TIMER_INST_GET(2));
nrfx_gpiote_t *const gpiote = &GPIOTE_NRFX_INST_BY_NODE(GPIOTE_NODE);

std::uint8_t sync_channel;
nrfx_gppi_handle_t sync_assert_connection;
nrfx_gppi_handle_t sync_release_connection;
atomic_t pulses;
onoff_client hf_client;
bool hf_reserved;

int fail(const char *stage, int error) {
  korora_debug::log("TIMEBASE_FAIL stage=%s status=%d\r\n", stage, error);
  return error;
}

void timer_handler(nrf_timer_event_t event, void *) {
  if (event == NRF_TIMER_EVENT_COMPARE0) {
    atomic_inc(&pulses);
  }
}

void timer_irq(const void *argument) {
  nrfx_timer_irq_handler(static_cast<const nrfx_timer_t *>(argument));
}

int acquire_hfxo() {
  onoff_manager *manager =
      z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
  if (manager == nullptr) {
    return -ENODEV;
  }
  sys_notify_init_spinwait(&hf_client.notify);
  int error = onoff_request(manager, &hf_client);
  if (error < 0) {
    return error;
  }
  int result = -EINPROGRESS;
  do {
    error = sys_notify_fetch_result(&hf_client.notify, &result);
  } while (error == -EAGAIN);
  if (error != 0 || result != 0) {
    return error != 0 ? error : result;
  }
  nrf_clock_hfclk_t source = NRF_CLOCK_HFCLK_LOW_ACCURACY;
  if (!nrf_clock_is_running(NRF_CLOCK, NRF_CLOCK_DOMAIN_HFCLK, &source) ||
      source != NRF_CLOCK_HFCLK_HIGH_ACCURACY) {
    (void)onoff_release(manager);
    return -EIO;
  }
  hf_reserved = true;
  return 0;
}

int configure_gpiote() {
#if KORORA_HAS_SYNC_DIR
  // U4 is a dedicated sync transmitter. Its /RE and DE pins are tied
  // together on SYNC_DIR, so keep the transceiver in transmit mode for the
  // lifetime of the timebase. The GPIOTE task below drives only SYNC_TX.
  nrf_gpio_cfg_output(SYNC_DIR_PIN);
  nrf_gpio_pin_set(SYNC_DIR_PIN);
#endif

  // Korora no longer captures external events or returned TTL edges. GPIOTE
  // is used only as a hardware task endpoint for the SYNC output, so there is
  // no GPIOTE input channel and no GPIOTE IRQ to enable here.
  if (!nrfx_gpiote_init_check(gpiote)) {
    const int error = nrfx_gpiote_init(gpiote, 0);
    if (error != 0) {
      return fail("gpiote.init", error);
    }
  }

  int error = nrfx_gpiote_channel_alloc(gpiote, &sync_channel);
  if (error != 0) {
    return fail("gpiote.sync_channel", error);
  }
  static nrfx_gpiote_output_config_t output{};
  output.drive = NRF_GPIO_PIN_S0S1;
  output.input_connect = NRF_GPIO_PIN_INPUT_DISCONNECT;
  output.pull = NRF_GPIO_PIN_NOPULL;
  nrfx_gpiote_task_config_t task{};
  task.task_ch = sync_channel;
  task.polarity = NRF_GPIOTE_POLARITY_TOGGLE;
  task.init_val = NRF_GPIOTE_INITIAL_VALUE_HIGH;
  error = nrfx_gpiote_output_configure(gpiote, SYNC_PIN, &output, &task);
  if (error != 0) {
    return fail("gpiote.sync_output", error);
  }
  nrfx_gpiote_out_task_enable(gpiote, SYNC_PIN);
  return 0;
}

int configure_timer() {
  IRQ_CONNECT(DT_IRQN(TIMER_NODE), DT_IRQ(TIMER_NODE, priority), timer_irq,
              &timer, 0);
  nrfx_timer_config_t config =
      NRFX_TIMER_DEFAULT_CONFIG(fairy::config::common_timer_hz);
  config.bit_width = NRF_TIMER_BIT_WIDTH_32;
  config.interrupt_priority = DT_IRQ(TIMER_NODE, priority);
  int error = nrfx_timer_init(&timer, &config, timer_handler);
  if (error != 0) {
    return fail("timer2.init", error);
  }
  nrfx_timer_extended_compare(&timer, NRF_TIMER_CC_CHANNEL0, period_ticks,
                              NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK, true);
  nrfx_timer_compare(&timer, NRF_TIMER_CC_CHANNEL1, pulse_width_ticks, false);

  error = nrfx_gppi_conn_alloc(
      nrfx_timer_compare_event_address_get(&timer, NRF_TIMER_CC_CHANNEL0),
      nrfx_gpiote_clr_task_address_get(gpiote, SYNC_PIN),
      &sync_assert_connection);
  if (error != 0) {
    return fail("ppi.sync_assert", error);
  }
  error = nrfx_gppi_conn_alloc(
      nrfx_timer_compare_event_address_get(&timer, NRF_TIMER_CC_CHANNEL1),
      nrfx_gpiote_set_task_address_get(gpiote, SYNC_PIN),
      &sync_release_connection);
  if (error != 0) {
    return fail("ppi.sync_release", error);
  }

  nrfx_gppi_conn_enable(sync_assert_connection);
  nrfx_gppi_conn_enable(sync_release_connection);
  nrf_timer_task_trigger(timer.p_reg, NRF_TIMER_TASK_CLEAR);
  nrf_timer_task_trigger(timer.p_reg, NRF_TIMER_TASK_START);
  return 0;
}

} // namespace

int initialize() {
  atomic_clear(&pulses);
  int error = acquire_hfxo();
  if (error != 0) {
    (void)fail("hfxo", error);
  }
  if (error == 0) {
    error = configure_gpiote();
  }
  if (error == 0) {
    error = configure_timer();
  }
#if KORORA_HAS_SYNC_DIR
  korora_debug::log(
      "TIMEBASE status=%d timer_hz=16000000 sync_hz=4 sync_pin=%u "
      "sync_dir_pin=%u captures=disabled\r\n",
      error, static_cast<unsigned int>(SYNC_PIN),
      static_cast<unsigned int>(SYNC_DIR_PIN));
#else
  korora_debug::log(
      "TIMEBASE status=%d timer_hz=16000000 sync_hz=4 sync_pin=%u "
      "captures=disabled\r\n",
      error, static_cast<unsigned int>(SYNC_PIN));
#endif
  return error;
}

std::uint64_t now() {
  const unsigned int key = irq_lock();
  std::uint32_t pulse = static_cast<std::uint32_t>(atomic_get(&pulses));
  nrf_timer_task_trigger(timer.p_reg, NRF_TIMER_TASK_CAPTURE2);
  const std::uint32_t phase =
      nrf_timer_cc_get(timer.p_reg, NRF_TIMER_CC_CHANNEL2);
  if (nrf_timer_event_check(timer.p_reg, NRF_TIMER_EVENT_COMPARE0) &&
      phase < period_ticks / 2U) {
    ++pulse;
  }
  irq_unlock(key);
  return static_cast<std::uint64_t>(pulse) * period_ticks + phase;
}

std::uint32_t pulse_count() {
  return static_cast<std::uint32_t>(atomic_get(&pulses));
}

std::uint64_t pulse_ticks(std::uint32_t pulse) {
  return static_cast<std::uint64_t>(pulse) * period_ticks;
}

} // namespace korora_time
