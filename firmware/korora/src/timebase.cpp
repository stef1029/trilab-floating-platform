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
#include "fairy_shared/static_queue.hpp"
#include "fairy_shared/system_config.hpp"

namespace korora_time {
namespace {

#define USER_NODE DT_PATH(zephyr_user)
#define TIMER_NODE DT_NODELABEL(timer2)
#define TTL_TIMER_NODE DT_NODELABEL(timer3)
#define SYNC_PIN NRF_DT_GPIOS_TO_PSEL(USER_NODE, sync_gpios)
#define EVENT_PIN NRF_DT_GPIOS_TO_PSEL(USER_NODE, event_gpios)
#define TTL_PIN NRF_DT_GPIOS_TO_PSEL(USER_NODE, ttl_input_gpios)
#define GPIOTE_NODE NRF_DT_GPIOTE_NODE(USER_NODE, sync_gpios)

inline constexpr std::uint32_t period_ticks =
    fairy::config::common_timer_hz / fairy::config::sync_rate_hz;
inline constexpr std::uint32_t pulse_width_ticks = 1600;

nrfx_timer_t timer = NRFX_TIMER_INSTANCE(NRF_TIMER_INST_GET(2));
nrfx_timer_t ttl_timer = NRFX_TIMER_INSTANCE(NRF_TIMER_INST_GET(3));
nrfx_gpiote_t *const gpiote = &GPIOTE_NRFX_INST_BY_NODE(GPIOTE_NODE);

std::uint8_t sync_channel;
std::uint8_t event_channel;
std::uint8_t ttl_channel;
nrfx_gppi_handle_t sync_assert_connection;
nrfx_gppi_handle_t sync_release_connection;
nrfx_gppi_handle_t event_capture_connection;
nrfx_gppi_handle_t ttl_capture_connection;
atomic_t pulses;
atomic_t event_sequence;
atomic_t ttl_sequence;
atomic_t ttl_drops;
fairy::StaticQueue<Capture, 32> captures;
k_spinlock capture_lock;
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

void empty_timer_handler(nrf_timer_event_t, void *) {}

void timer_irq(const void *argument) {
  nrfx_timer_irq_handler(static_cast<const nrfx_timer_t *>(argument));
}

std::uint64_t from_phase(std::uint32_t phase) {
  const unsigned int key = irq_lock();
  std::uint32_t pulse = static_cast<std::uint32_t>(atomic_get(&pulses));
  nrf_timer_task_trigger(timer.p_reg, NRF_TIMER_TASK_CAPTURE2);
  const std::uint32_t current =
      nrf_timer_cc_get(timer.p_reg, NRF_TIMER_CC_CHANNEL2);
  const bool boundary =
      nrf_timer_event_check(timer.p_reg, NRF_TIMER_EVENT_COMPARE0);
  if (boundary && current < period_ticks / 2U) {
    ++pulse;
  }
  if (phase > current && phase - current > period_ticks / 2U && pulse > 0U) {
    --pulse;
  }
  irq_unlock(key);
  return static_cast<std::uint64_t>(pulse) * period_ticks + phase;
}

void queue_capture(CaptureKind kind, std::uint64_t ticks,
                   std::uint32_t sequence) {
  const k_spinlock_key_t key = k_spin_lock(&capture_lock);
  if (!captures.push(Capture{kind, ticks, sequence}) &&
      kind == CaptureKind::ttl_input) {
    atomic_inc(&ttl_drops);
  }
  k_spin_unlock(&capture_lock, key);
}

void event_handler(nrfx_gpiote_pin_t, nrfx_gpiote_trigger_t, void *) {
  const std::uint32_t phase =
      nrf_timer_cc_get(timer.p_reg, NRF_TIMER_CC_CHANNEL3);
  queue_capture(CaptureKind::external_event, from_phase(phase),
                static_cast<std::uint32_t>(atomic_inc(&event_sequence) + 1));
}

void ttl_handler(nrfx_gpiote_pin_t, nrfx_gpiote_trigger_t, void *) {
  const std::uint32_t phase =
      nrf_timer_cc_get(ttl_timer.p_reg, NRF_TIMER_CC_CHANNEL0);
  queue_capture(CaptureKind::ttl_input, from_phase(phase),
                static_cast<std::uint32_t>(atomic_inc(&ttl_sequence) + 1));
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
  IRQ_CONNECT(DT_IRQN(GPIOTE_NODE), DT_IRQ(GPIOTE_NODE, priority),
              nrfx_gpiote_irq_handler, gpiote, 0);
  if (!nrfx_gpiote_init_check(gpiote)) {
    const int error = nrfx_gpiote_init(gpiote, 0);
    if (error != 0) {
      return fail("gpiote.init", error);
    }
  }
  irq_enable(DT_IRQN(GPIOTE_NODE));

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

  static const nrf_gpio_pin_pull_t pull = NRF_GPIO_PIN_PULLDOWN;
  error = nrfx_gpiote_channel_alloc(gpiote, &event_channel);
  if (error != 0) {
    return fail("gpiote.event_channel", error);
  }
  nrfx_gpiote_trigger_config_t event_trigger{};
  event_trigger.trigger = NRFX_GPIOTE_TRIGGER_LOTOHI;
  event_trigger.p_in_channel = &event_channel;
  nrfx_gpiote_handler_config_t event_handler_config{};
  event_handler_config.handler = event_handler;
  event_handler_config.p_context = nullptr;
  nrfx_gpiote_input_pin_config_t event_input{};
  event_input.p_pull_config = &pull;
  event_input.p_trigger_config = &event_trigger;
  event_input.p_handler_config = &event_handler_config;
  error = nrfx_gpiote_input_configure(gpiote, EVENT_PIN, &event_input);
  if (error != 0) {
    return fail("gpiote.event_input", error);
  }

  error = nrfx_gpiote_channel_alloc(gpiote, &ttl_channel);
  if (error != 0) {
    return fail("gpiote.ttl_channel", error);
  }
  nrfx_gpiote_trigger_config_t ttl_trigger{};
  ttl_trigger.trigger = NRFX_GPIOTE_TRIGGER_LOTOHI;
  ttl_trigger.p_in_channel = &ttl_channel;
  nrfx_gpiote_handler_config_t ttl_handler_config{};
  ttl_handler_config.handler = ttl_handler;
  ttl_handler_config.p_context = nullptr;
  nrfx_gpiote_input_pin_config_t ttl_input{};
  ttl_input.p_pull_config = &pull;
  ttl_input.p_trigger_config = &ttl_trigger;
  ttl_input.p_handler_config = &ttl_handler_config;
  error = nrfx_gpiote_input_configure(gpiote, TTL_PIN, &ttl_input);
  if (error != 0) {
    return fail("gpiote.ttl_input", error);
  }
  return 0;
}

int configure_timers() {
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

  IRQ_CONNECT(DT_IRQN(TTL_TIMER_NODE), DT_IRQ(TTL_TIMER_NODE, priority),
              timer_irq, &ttl_timer, 0);
  config.interrupt_priority = DT_IRQ(TTL_TIMER_NODE, priority);
  error = nrfx_timer_init(&ttl_timer, &config, empty_timer_handler);
  if (error != 0) {
    return fail("timer3.init", error);
  }
  nrf_timer_task_trigger(ttl_timer.p_reg, NRF_TIMER_TASK_CLEAR);
  nrf_timer_task_trigger(ttl_timer.p_reg, NRF_TIMER_TASK_START);

  error = nrfx_gppi_conn_alloc(
      nrfx_timer_compare_event_address_get(&timer, NRF_TIMER_CC_CHANNEL0),
      nrfx_gpiote_clr_task_address_get(gpiote, SYNC_PIN),
      &sync_assert_connection);
  if (error != 0) {
    return fail("ppi.sync_assert", error);
  }
  error = nrfx_gppi_ep_attach(
      nrf_timer_task_address_get(ttl_timer.p_reg, NRF_TIMER_TASK_CLEAR),
      sync_assert_connection);
  if (error != 0) {
    return fail("ppi.sync_assert_fork", error);
  }
  error = nrfx_gppi_conn_alloc(
      nrfx_timer_compare_event_address_get(&timer, NRF_TIMER_CC_CHANNEL1),
      nrfx_gpiote_set_task_address_get(gpiote, SYNC_PIN),
      &sync_release_connection);
  if (error != 0) {
    return fail("ppi.sync_release", error);
  }
  error = nrfx_gppi_conn_alloc(
      nrfx_gpiote_in_event_address_get(gpiote, EVENT_PIN),
      nrf_timer_task_address_get(timer.p_reg, NRF_TIMER_TASK_CAPTURE3),
      &event_capture_connection);
  if (error != 0) {
    return fail("ppi.event_capture", error);
  }
  error = nrfx_gppi_conn_alloc(
      nrfx_gpiote_in_event_address_get(gpiote, TTL_PIN),
      nrf_timer_task_address_get(ttl_timer.p_reg, NRF_TIMER_TASK_CAPTURE0),
      &ttl_capture_connection);
  if (error != 0) {
    return fail("ppi.ttl_capture", error);
  }

  nrfx_gppi_conn_enable(sync_assert_connection);
  nrfx_gppi_conn_enable(sync_release_connection);
  nrfx_gppi_conn_enable(event_capture_connection);
  nrfx_gppi_conn_enable(ttl_capture_connection);
  nrfx_gpiote_trigger_enable(gpiote, EVENT_PIN, true);
  nrfx_gpiote_trigger_enable(gpiote, TTL_PIN, true);
  nrf_timer_task_trigger(timer.p_reg, NRF_TIMER_TASK_CLEAR);
  nrf_timer_task_trigger(timer.p_reg, NRF_TIMER_TASK_START);
  return 0;
}

} // namespace

int initialize() {
  atomic_clear(&pulses);
  atomic_clear(&event_sequence);
  atomic_clear(&ttl_sequence);
  atomic_clear(&ttl_drops);
  int error = acquire_hfxo();
  if (error != 0) {
    (void)fail("hfxo", error);
  }
  if (error == 0) {
    error = configure_gpiote();
  }
  if (error == 0) {
    error = configure_timers();
  }
  korora_debug::log(
      "TIMEBASE status=%d timer_hz=16000000 sync_hz=4 event_pin=%u "
      "ttl_pin=%u\r\n",
      error, static_cast<unsigned int>(EVENT_PIN),
      static_cast<unsigned int>(TTL_PIN));
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

bool pop_capture(Capture &capture) {
  const k_spinlock_key_t key = k_spin_lock(&capture_lock);
  const bool result = captures.pop(capture);
  k_spin_unlock(&capture_lock, key);
  return result;
}

std::uint32_t ttl_capture_count() {
  return static_cast<std::uint32_t>(atomic_get(&ttl_sequence));
}

std::uint32_t ttl_capture_drops() {
  return static_cast<std::uint32_t>(atomic_get(&ttl_drops));
}

} // namespace korora_time
