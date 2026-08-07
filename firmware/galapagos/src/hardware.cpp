#include "hardware.hpp"

#include <cerrno>

#include <gpiote_nrfx.h>
#include <hal/nrf_gpio.h>
#include <helpers/nrfx_gppi.h>
#include <nrfx_gpiote.h>
#include <nrfx_grtc.h>

#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "debug_log.hpp"
#include "fairy_shared/fairy_protocol.hpp"
#include "fairy_shared/static_queue.hpp"
#include "fairy_shared/tlv.hpp"
#include "record_stream.hpp"

namespace galapagos_hardware {
namespace {

#define USER_NODE DT_PATH(zephyr_user)
#define EVENT_INPUT_PIN NRF_DT_GPIOS_TO_PSEL(USER_NODE, event_gpios)
#define EVENT_GPIOTE_NODE NRF_DT_GPIOTE_NODE(USER_NODE, event_gpios)
#define TTL_OUTPUT_PIN NRF_DT_GPIOS_TO_PSEL(USER_NODE, ttl_output_gpios)
#define TTL_GPIOTE_NODE NRF_DT_GPIOTE_NODE(USER_NODE, ttl_output_gpios)

BUILD_ASSERT(DT_SAME_NODE(EVENT_GPIOTE_NODE, TTL_GPIOTE_NODE),
             "event and TTL pins must use the same GPIOTE instance");

nrfx_gpiote_t *const gpiote = &GPIOTE_NRFX_INST_BY_NODE(EVENT_GPIOTE_NODE);
std::uint8_t event_gpiote_channel;
std::uint8_t event_grtc_channel;
nrfx_gppi_handle_t event_connection;
std::uint8_t ttl_gpiote_channel;
std::uint8_t ttl_set_channel;
std::uint8_t ttl_clear_channel;
nrfx_gppi_handle_t ttl_set_connection;
nrfx_gppi_handle_t ttl_clear_connection;
atomic_t pulse_armed;
atomic_t active_session;
atomic_t event_sequence;

struct Pulse {
  std::uint32_t sequence{};
  std::uint64_t target_us{};
  std::uint32_t width_us{};
};

Pulse pulse;
fairy::StaticQueue<Pulse, 8> pending_pulses;
k_spinlock pulse_lock;
std::uint64_t last_queued_target_us;

void program_pulse(const Pulse &next) {
  pulse = next;
  nrfx_gppi_conn_enable(ttl_set_connection);
  nrfx_gppi_conn_enable(ttl_clear_connection);
  /*
   * The optimized GRTC setters require interrupts to be locked. Enabling the
   * channels and using safe setting also prevents a prior compare state from
   * being interpreted as the newly queued pulse.
   */
  const unsigned int key = irq_lock();
  (void)nrfx_grtc_syscounter_cc_int_enable(ttl_set_channel);
  (void)nrfx_grtc_syscounter_cc_int_enable(ttl_clear_channel);
  nrfx_grtc_syscounter_cc_abs_set(ttl_set_channel, pulse.target_us, true);
  nrfx_grtc_syscounter_cc_abs_set(ttl_clear_channel,
                                  pulse.target_us + pulse.width_us, true);
  irq_unlock(key);
}

int wait_for_grtc() {
  nrfx_grtc_active_request_set(true);
  for (std::uint32_t attempt = 0; attempt < 10'000; ++attempt) {
    if (nrfx_grtc_ready_check()) {
      return 0;
    }
    k_busy_wait(10);
  }
  return -ETIMEDOUT;
}

void gpiote_irq_wrapper(const void *argument) {
  nrfx_gpiote_irq_handler(
      static_cast<nrfx_gpiote_t *>(const_cast<void *>(argument)));
}

void event_handler(nrfx_gpiote_pin_t, nrfx_gpiote_trigger_t, void *) {
  std::uint64_t captured_us{};
  if (nrfx_grtc_syscounter_cc_value_read(event_grtc_channel, &captured_us) !=
      0) {
    return;
  }
  const std::uint32_t sequence =
      static_cast<std::uint32_t>(atomic_inc(&event_sequence) + 1);
  std::uint8_t payload[24]{};
  fairy::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.u32(static_cast<std::uint16_t>(fairy::protocol::Field::sequence),
                   sequence);
  (void)fields.u8(static_cast<std::uint16_t>(fairy::protocol::Field::state), 1);
  (void)galapagos_stream::publish_record(
      fairy::protocol::RecordType::digital_input,
      fairy::protocol::critical | fairy::protocol::actual_time,
      captured_us * 16ULL,
      static_cast<std::uint32_t>(atomic_get(&active_session)), payload,
      fields.size());
}

void ttl_set_handler(std::int32_t id, std::uint64_t cc_value, void *) {
  if (id != static_cast<std::int32_t>(ttl_set_channel)) {
    return;
  }
  std::uint8_t payload[40]{};
  fairy::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.u32(static_cast<std::uint16_t>(fairy::protocol::Field::sequence),
                   pulse.sequence);
  (void)fields.u64(
      static_cast<std::uint16_t>(fairy::protocol::Field::requested_ticks),
      pulse.target_us * 16ULL);
  (void)fields.u64(
      static_cast<std::uint16_t>(fairy::protocol::Field::actual_ticks),
      cc_value * 16ULL);
  (void)fields.u32(
      static_cast<std::uint16_t>(fairy::protocol::Field::duration_us),
      pulse.width_us);
  (void)galapagos_stream::publish_record(
      fairy::protocol::RecordType::ttl_generated,
      fairy::protocol::critical | fairy::protocol::actual_time |
          fairy::protocol::scheduled,
      cc_value * 16ULL, static_cast<std::uint32_t>(atomic_get(&active_session)),
      payload, fields.size());
}

void ttl_clear_handler(std::int32_t id, std::uint64_t, void *) {
  if (id != static_cast<std::int32_t>(ttl_clear_channel)) {
    return;
  }
  Pulse next;
  const k_spinlock_key_t key = k_spin_lock(&pulse_lock);
  const bool have_next = pending_pulses.pop(next);
  if (!have_next) {
    atomic_clear(&pulse_armed);
    last_queued_target_us = 0;
  } else {
    program_pulse(next);
  }
  k_spin_unlock(&pulse_lock, key);
}

int configure_event() {
  int error = nrfx_grtc_channel_alloc(&event_grtc_channel);
  if (error != 0) {
    return error;
  }
  error = nrfx_grtc_syscounter_capture(event_grtc_channel);
  if (error != 0) {
    return error;
  }

  IRQ_CONNECT(DT_IRQN(EVENT_GPIOTE_NODE), DT_IRQ(EVENT_GPIOTE_NODE, priority),
              gpiote_irq_wrapper, gpiote, 0);
  if (!nrfx_gpiote_init_check(gpiote)) {
    error = nrfx_gpiote_init(gpiote, DT_IRQ(EVENT_GPIOTE_NODE, priority));
    if (error != 0) {
      return error;
    }
  }
  irq_enable(DT_IRQN(EVENT_GPIOTE_NODE));
  error = nrfx_gpiote_channel_alloc(gpiote, &event_gpiote_channel);
  if (error != 0) {
    return error;
  }

  static const nrf_gpio_pin_pull_t pull = NRF_GPIO_PIN_PULLDOWN;
  nrfx_gpiote_trigger_config_t trigger{};
  trigger.trigger = NRFX_GPIOTE_TRIGGER_LOTOHI;
  trigger.p_in_channel = &event_gpiote_channel;
  nrfx_gpiote_handler_config_t handler{};
  handler.handler = event_handler;
  handler.p_context = nullptr;
  nrfx_gpiote_input_pin_config_t input{};
  input.p_pull_config = &pull;
  input.p_trigger_config = &trigger;
  input.p_handler_config = &handler;
  error = nrfx_gpiote_input_configure(gpiote, EVENT_INPUT_PIN, &input);
  if (error != 0) {
    return error;
  }
  error = nrfx_gppi_conn_alloc(
      nrfx_gpiote_in_event_address_get(gpiote, EVENT_INPUT_PIN),
      nrfx_grtc_capture_task_address_get(event_grtc_channel),
      &event_connection);
  if (error != 0) {
    return error;
  }
  nrfx_gppi_conn_enable(event_connection);
  nrfx_gpiote_trigger_enable(gpiote, EVENT_INPUT_PIN, true);
  return 0;
}

int configure_ttl() {
#if !defined(GPIOTE_FEATURE_SET_PRESENT) || !defined(GPIOTE_FEATURE_CLR_PRESENT)
#error "Galapagos TTL requires GPIOTE SET and CLR tasks"
#endif
  int error = nrfx_gpiote_channel_alloc(gpiote, &ttl_gpiote_channel);
  if (error != 0) {
    return error;
  }
  static nrfx_gpiote_output_config_t output{};
  output.drive = NRF_GPIO_PIN_S0S1;
  output.input_connect = NRF_GPIO_PIN_INPUT_CONNECT;
  output.pull = NRF_GPIO_PIN_NOPULL;
  nrfx_gpiote_task_config_t task{};
  task.task_ch = ttl_gpiote_channel;
  task.polarity = NRF_GPIOTE_POLARITY_TOGGLE;
  task.init_val = NRF_GPIOTE_INITIAL_VALUE_LOW;
  error = nrfx_gpiote_output_configure(gpiote, TTL_OUTPUT_PIN, &output, &task);
  if (error != 0) {
    return error;
  }
  nrfx_gpiote_out_task_enable(gpiote, TTL_OUTPUT_PIN);
  nrfx_gpiote_clr_task_trigger(gpiote, TTL_OUTPUT_PIN);

  if ((error = nrfx_grtc_channel_alloc(&ttl_set_channel)) != 0 ||
      (error = nrfx_grtc_channel_alloc(&ttl_clear_channel)) != 0) {
    return error;
  }
  nrfx_grtc_channel_callback_set(ttl_set_channel, ttl_set_handler, nullptr);
  nrfx_grtc_channel_callback_set(ttl_clear_channel, ttl_clear_handler, nullptr);
  error = nrfx_gppi_conn_alloc(
      nrfx_grtc_event_compare_address_get(ttl_set_channel),
      nrfx_gpiote_set_task_address_get(gpiote, TTL_OUTPUT_PIN),
      &ttl_set_connection);
  if (error != 0) {
    return error;
  }
  error = nrfx_gppi_conn_alloc(
      nrfx_grtc_event_compare_address_get(ttl_clear_channel),
      nrfx_gpiote_clr_task_address_get(gpiote, TTL_OUTPUT_PIN),
      &ttl_clear_connection);
  if (error != 0) {
    return error;
  }
  nrfx_gppi_conn_enable(ttl_set_connection);
  nrfx_gppi_conn_enable(ttl_clear_connection);
  return 0;
}

} // namespace

int initialize() {
  atomic_clear(&pulse_armed);
  atomic_clear(&active_session);
  atomic_clear(&event_sequence);
  pending_pulses.clear();
  last_queued_target_us = 0;
  int error = wait_for_grtc();
  if (error == 0) {
    error = configure_event();
  }
  if (error == 0) {
    error = configure_ttl();
  }
  galapagos_debug::log(
      "HARDWARE galapagos event_pin=%u ttl_pin=%u status=%d\r\n",
      static_cast<unsigned int>(EVENT_INPUT_PIN),
      static_cast<unsigned int>(TTL_OUTPUT_PIN), error);
  return error;
}

std::uint64_t now_ticks() { return nrfx_grtc_syscounter_get() * 16ULL; }

void set_session(std::uint32_t session_id) {
  atomic_set(&active_session, static_cast<atomic_val_t>(session_id));
}

fairy::protocol::Status schedule_ttl(std::uint32_t sequence,
                                     std::uint64_t target_ticks,
                                     std::uint32_t width_us) {
  if (target_ticks % 16ULL != 0ULL || width_us == 0U || width_us > 2'000'000U) {
    return fairy::protocol::Status::invalid_parameter;
  }
  const std::uint64_t target_us = target_ticks / 16ULL;
  const std::uint64_t now_us = nrfx_grtc_syscounter_get();

  const std::int64_t lead_us =
      target_us >= now_us ? static_cast<std::int64_t>(target_us - now_us)
                          : -static_cast<std::int64_t>(now_us - target_us);

  if (target_us <= now_us || target_us - now_us < 50'000ULL ||
      UINT64_MAX - target_us < width_us) {
    galapagos_debug::log("TTL_REJECT sequence=%u target_us=%llu now_us=%llu "
                         "lead_us=%lld width_us=%u modulo=%u\r\n",
                         sequence, static_cast<unsigned long long>(target_us),
                         static_cast<unsigned long long>(now_us),
                         static_cast<long long>(lead_us), width_us,
                         static_cast<unsigned int>(target_ticks % 16ULL));

    return fairy::protocol::Status::invalid_parameter;
  }
  const k_spinlock_key_t key = k_spin_lock(&pulse_lock);
  if (atomic_get(&pulse_armed) != 0) {
    if (target_us <= last_queued_target_us ||
        !pending_pulses.push(Pulse{sequence, target_us, width_us})) {
      k_spin_unlock(&pulse_lock, key);
      return fairy::protocol::Status::queue_full;
    }
    last_queued_target_us = target_us;
    k_spin_unlock(&pulse_lock, key);
    return fairy::protocol::Status::accepted;
  }
  atomic_set(&pulse_armed, 1);
  const Pulse first{sequence, target_us, width_us};
  last_queued_target_us = target_us;
  program_pulse(first);
  k_spin_unlock(&pulse_lock, key);
  return fairy::protocol::Status::accepted;
}

void force_ttl_low() {
  const k_spinlock_key_t key = k_spin_lock(&pulse_lock);
  nrfx_gppi_conn_disable(ttl_set_connection);
  nrfx_gppi_conn_disable(ttl_clear_connection);
  (void)nrfx_grtc_syscounter_cc_int_disable(ttl_set_channel);
  (void)nrfx_grtc_syscounter_cc_int_disable(ttl_clear_channel);
  (void)nrfx_grtc_syscounter_cc_disable(ttl_set_channel);
  (void)nrfx_grtc_syscounter_cc_disable(ttl_clear_channel);
  pending_pulses.clear();
  last_queued_target_us = 0;
  atomic_clear(&pulse_armed);
  nrfx_gpiote_clr_task_trigger(gpiote, TTL_OUTPUT_PIN);
  k_spin_unlock(&pulse_lock, key);
}

bool ttl_armed() { return atomic_get(&pulse_armed) != 0; }

} // namespace galapagos_hardware
