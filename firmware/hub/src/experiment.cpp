#include "experiment.hpp"

#include <array>

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>

#include "ble_gateway.hpp"
#include "control.hpp"
#include "node_manager.hpp"
#include "rig_protocol/command_protocol.hpp"
#include "rig_protocol/record_protocol.hpp"
#include "rig_protocol/system_config.hpp"
#include "rig_protocol/tlv.hpp"
#include "ttl_generator_manager.hpp"
#include "rs485_bus.hpp"
#include "timebase.hpp"

namespace hub_experiment {
namespace {

/* Keep five or fewer pulses queued at 10 Hz (TTL generator holds nine total). */
inline constexpr std::uint64_t scheduling_lead_ticks = 6'400'000ULL;
inline constexpr std::uint64_t minimum_delivery_lead_ticks = 1'600'000ULL;
inline constexpr std::uint32_t default_test_interval_ms = 5000;
inline constexpr std::uint8_t sync_test_valve_address = 0x14U;
inline constexpr std::uint32_t sync_test_valve_duration_ms = 150U;

struct PendingPulse {
  bool used{};
  std::uint32_t sequence{};
  std::uint32_t width_us{};
  std::uint64_t target_ticks{};
};

std::array<PendingPulse, 16> pending;
k_mutex state_mutex;
K_SEM_DEFINE(start_sem, 0, 1);
atomic_t session;
atomic_t ttl_enabled;
atomic_t test_enabled;
std::uint32_t ttl_frequency_millihz;
std::uint32_t ttl_width_us;
std::uint32_t ttl_remaining;
std::uint32_t ttl_in_flight;
bool ttl_finite;
atomic_t sequence_counter;
std::uint64_t next_target;
std::uint32_t test_command_interval_ms;
std::int64_t next_test_command_ms;
atomic_t record_id;
std::uint64_t last_health_ticks;
rig::protocol::TelemetryLevel telemetry_before_test;

void publish(rig::protocol::RecordType type, std::uint16_t flags,
             std::uint64_t ticks, const std::uint8_t *payload,
             std::size_t payload_length, bool diagnostic_priority = false) {
  rig::protocol::RecordHeader header;
  header.type = type;
  header.flags = flags;
  header.record_id = static_cast<std::uint32_t>(atomic_inc(&record_id) + 1);
  header.session_id = static_cast<std::uint32_t>(atomic_get(&session));
  header.timestamp_ticks = ticks;
  header.clock_hz = rig::config::common_timer_hz;
  std::uint8_t encoded[rig::protocol::max_record_size]{};
  const std::size_t length = rig::protocol::encode_record(
      header, payload, payload_length, encoded, sizeof(encoded));
  if (length != 0U) {
    (void)hub_ble::send_to_host(rig::config::hub_address,
                                     rig::transport::Channel::record, encoded,
                                     length, 0, 0, 0, diagnostic_priority);
  }
}

void record_schedule(std::uint32_t sequence, std::uint64_t target_ticks,
                     std::uint32_t width_us) {
  std::uint8_t payload[40]{};
  rig::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.u32(static_cast<std::uint16_t>(rig::protocol::Field::sequence),
                   sequence);
  if (target_ticks != 0U) {
    (void)fields.u64(
        static_cast<std::uint16_t>(rig::protocol::Field::requested_ticks),
        target_ticks);
  }
  (void)fields.u32(
      static_cast<std::uint16_t>(rig::protocol::Field::duration_us),
      width_us);
  publish(rig::protocol::RecordType::ttl_scheduled,
          rig::protocol::critical | rig::protocol::scheduled, target_ticks,
          payload, fields.size(), true);
}

void publish_health(std::uint64_t now) {
  if (now - last_health_ticks < rig::config::common_timer_hz) {
    return;
  }
  last_health_ticks = now;
  if (hub_nodes::telemetry() ==
      rig::protocol::TelemetryLevel::critical) {
    return;
  }
  const hub_rs485::Diagnostics rs485 = hub_rs485::diagnostics();
  std::uint8_t payload[144]{};
  rig::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.u32(
      static_cast<std::uint16_t>(rig::protocol::Field::uptime_ms),
      static_cast<std::uint32_t>(now / 16'000ULL));
  (void)fields.u32(
      static_cast<std::uint16_t>(rig::protocol::Field::transport_errors),
      rs485.errors);
  (void)fields.u32(
      static_cast<std::uint16_t>(rig::protocol::Field::retry_count),
      rs485.retries);
  (void)fields.u32(
      static_cast<std::uint16_t>(rig::protocol::Field::timeout_count),
      rs485.timeouts);
  (void)fields.u32(static_cast<std::uint16_t>(
                       rig::protocol::Field::transport_decode_errors),
                   rs485.decode_errors);
  (void)fields.u32(static_cast<std::uint16_t>(
                       rig::protocol::Field::transport_reassembly_errors),
                   rs485.reassembly_errors);
  (void)fields.u32(static_cast<std::uint16_t>(
                       rig::protocol::Field::transport_transmit_errors),
                   rs485.transmit_errors);
  (void)fields.u32(
      static_cast<std::uint16_t>(rig::protocol::Field::dropped_records),
      hub_ble::dropped_to_host());
  (void)fields.u32(static_cast<std::uint16_t>(rig::protocol::Field::detail),
                   hub_ble::dropped_to_ttl_generator());
  (void)fields.i32(static_cast<std::uint16_t>(rig::protocol::Field::rssi_dbm),
                   hub_ble::ttl_generator_rssi());
  (void)fields.boolean(
      static_cast<std::uint16_t>(rig::protocol::Field::state),
      hub_ble::ttl_generator_connected());
  publish(rig::protocol::RecordType::link_quality, 0, now, payload,
          fields.size());
}

PendingPulse *allocate_pending(std::uint32_t sequence,
                               std::uint64_t target_ticks,
                               std::uint32_t width_us) {
  for (PendingPulse &pulse : pending) {
    if (!pulse.used) {
      pulse = {true, sequence, width_us, target_ticks};
      return &pulse;
    }
  }
  return nullptr;
}

PendingPulse *find_pending(std::uint32_t sequence) {
  for (PendingPulse &pulse : pending) {
    if (pulse.used && pulse.sequence == sequence) {
      return &pulse;
    }
  }
  return nullptr;
}

bool send_ttl_command(std::uint32_t sequence, std::uint64_t target_ticks,
                      std::uint32_t width_us) {
  std::uint64_t ttl_generator_ticks{};
  std::uint64_t ttl_generator_now{};
  if (!hub_ttl_generator::hub_to_local(target_ticks, ttl_generator_ticks) ||
      !hub_ttl_generator::hub_to_local(hub_time::now(), ttl_generator_now) ||
      ttl_generator_ticks <= ttl_generator_now + minimum_delivery_lead_ticks) {
    return false;
  }
  ttl_generator_ticks = (ttl_generator_ticks / 16ULL) * 16ULL;

  std::uint8_t fields_buffer[24]{};
  rig::protocol::TlvWriter fields(fields_buffer, sizeof(fields_buffer));
  (void)fields.u32(
      static_cast<std::uint16_t>(rig::protocol::CommandField::ttl_width_us),
      width_us);
  (void)fields.u32(
      static_cast<std::uint16_t>(rig::protocol::CommandField::sequence),
      sequence);
  rig::protocol::MessageHeader header;
  header.opcode = rig::protocol::Opcode::schedule_ttl;
  header.flags = rig::protocol::require_response;
  header.command_id = sequence;
  header.session_id = static_cast<std::uint32_t>(atomic_get(&session));
  header.execute_at_ticks = ttl_generator_ticks;
  header.deadline_ms = 2000;
  std::uint8_t message[rig::protocol::command_max_message_size]{};
  const std::size_t length = rig::protocol::encode_command(
      header, fields_buffer, fields.size(), message, sizeof(message));
  if (length == 0U) {
    return false;
  }

  k_mutex_lock(&state_mutex, K_FOREVER);
  PendingPulse *pulse = allocate_pending(sequence, target_ticks, width_us);
  if (pulse != nullptr) {
    ++ttl_in_flight;
  }
  k_mutex_unlock(&state_mutex);
  if (pulse == nullptr) {
    return false;
  }

  if (hub_ble::send_to_ttl_generator(rig::transport::Channel::command, message,
                                    length)) {
    return true;
  }

  k_mutex_lock(&state_mutex, K_FOREVER);
  pulse = find_pending(sequence);
  if (pulse != nullptr) {
    pulse->used = false;
    if (ttl_in_flight != 0U) {
      --ttl_in_flight;
    }
  }
  k_mutex_unlock(&state_mutex);
  return false;
}

void send_sync_test_valve_command() {
  std::uint8_t field_buffer[16]{};
  rig::protocol::TlvWriter fields(field_buffer, sizeof(field_buffer));
  (void)fields.u32(
      static_cast<std::uint16_t>(rig::protocol::CommandField::duration_ms),
      sync_test_valve_duration_ms);

  rig::protocol::MessageHeader header;
  header.opcode = rig::protocol::Opcode::actuate_valve;
  header.flags = rig::protocol::require_response |
                 rig::protocol::execute_immediately |
                 rig::protocol::safety_authorized;
  header.command_id =
      static_cast<std::uint32_t>(atomic_inc(&sequence_counter) + 1);
  header.session_id = static_cast<std::uint32_t>(atomic_get(&session));

  std::uint8_t message[rig::protocol::command_max_message_size]{};
  const std::size_t length = rig::protocol::encode_command(
      header, field_buffer, fields.size(), message, sizeof(message));
  if (length != 0U) {
    (void)hub_nodes::queue_command(sync_test_valve_address, message,
                                        length, 0);
  }
}

void worker(void *, void *, void *) {
  k_sem_take(&start_sem, K_FOREVER);
  while (true) {
    const std::uint64_t now = hub_time::now();
    publish_health(now);
    k_mutex_lock(&state_mutex, K_FOREVER);
    if (atomic_get(&ttl_enabled) != 0 && next_target != 0U &&
        (!ttl_finite || ttl_remaining > ttl_in_flight) &&
        now + scheduling_lead_ticks >= next_target) {
      const std::uint32_t frequency = ttl_frequency_millihz;
      const std::uint32_t width = ttl_width_us;
      const std::uint32_t sequence =
          static_cast<std::uint32_t>(atomic_inc(&sequence_counter) + 1);
      const std::uint64_t target = next_target;
      k_mutex_unlock(&state_mutex);
      const bool sent = send_ttl_command(sequence, target, width);
      k_mutex_lock(&state_mutex, K_FOREVER);
      if (sent) {
        next_target += 16'000'000'000ULL / frequency;
      } else {
        next_target = hub_time::now() + scheduling_lead_ticks * 2ULL;
      }
      k_mutex_unlock(&state_mutex);
    } else {
      k_mutex_unlock(&state_mutex);
    }

    if (atomic_get(&test_enabled) != 0 &&
        k_uptime_get() >= next_test_command_ms) {
      send_sync_test_valve_command();
      next_test_command_ms =
          k_uptime_get() + static_cast<std::int64_t>(test_command_interval_ms);
    }
    k_sleep(K_MSEC(1));
  }
}

K_THREAD_DEFINE(worker_id, 4096, worker, nullptr, nullptr, nullptr, 8, 0, 0);

} // namespace

void initialize() {
  k_mutex_init(&state_mutex);
  atomic_clear(&session);
  atomic_clear(&ttl_enabled);
  atomic_clear(&test_enabled);
  ttl_frequency_millihz = 0;
  ttl_width_us = 100;
  ttl_remaining = 0;
  ttl_in_flight = 0;
  ttl_finite = false;
  atomic_clear(&sequence_counter);
  next_target = 0;
  test_command_interval_ms = default_test_interval_ms;
  next_test_command_ms = 0;
  atomic_clear(&record_id);
  last_health_ticks = 0;
  telemetry_before_test = rig::protocol::TelemetryLevel::standard;
  for (PendingPulse &pulse : pending) {
    pulse = {};
  }
  k_sem_give(&start_sem);
}

void host_connected() {
  std::uint8_t payload[64]{};
  rig::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.string(
      static_cast<std::uint16_t>(rig::protocol::Field::board_kind),
      "nrf52840dk");
  (void)fields.string(
      static_cast<std::uint16_t>(rig::protocol::Field::firmware_version),
      "3.0.0");
  publish(rig::protocol::RecordType::boot, rig::protocol::first_after_reset,
          hub_time::now(), payload, fields.size());
}

void publish_fault(const char *reason) {
  std::uint8_t payload[96]{};
  rig::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.string(
      static_cast<std::uint16_t>(rig::protocol::Field::reason), reason);
  publish(rig::protocol::RecordType::fault,
          rig::protocol::critical | rig::protocol::loss_latched,
          hub_time::now(), payload, fields.size(), true);
}

void set_session(std::uint32_t session_id) {
  atomic_set(&session, static_cast<atomic_val_t>(session_id));
  if (session_id == 0U) {
    stop_ttl_train();
    stop_sync_test();
  }
}

bool start_ttl_train(std::uint32_t frequency_millihz, std::uint32_t width_us,
                     std::uint32_t count) {
  const std::uint64_t period_us =
      frequency_millihz == 0U ? 0U : 1'000'000'000ULL / frequency_millihz;
  if (frequency_millihz < 100U || frequency_millihz > 10'000U ||
      width_us == 0U || width_us > 2'000'000U || width_us >= period_us ||
      atomic_get(&session) == 0) {
    return false;
  }
  k_mutex_lock(&state_mutex, K_FOREVER);
  ttl_frequency_millihz = frequency_millihz;
  ttl_width_us = width_us;
  ttl_remaining = count;
  ttl_in_flight = 0;
  ttl_finite = count != 0U;
  next_target = hub_time::now() + scheduling_lead_ticks * 2ULL;
  atomic_set(&ttl_enabled, 1);
  k_mutex_unlock(&state_mutex);
  return true;
}

void stop_ttl_train() {
  const std::uint32_t current_session =
      static_cast<std::uint32_t>(atomic_get(&session));
  k_mutex_lock(&state_mutex, K_FOREVER);
  const std::uint32_t stop_command =
      static_cast<std::uint32_t>(atomic_inc(&sequence_counter) + 1);
  atomic_clear(&ttl_enabled);
  ttl_frequency_millihz = 0;
  ttl_finite = false;
  ttl_remaining = 0;
  ttl_in_flight = 0;
  next_target = 0;
  for (PendingPulse &pulse : pending) {
    pulse.used = false;
  }
  k_mutex_unlock(&state_mutex);
  if (current_session != 0U && hub_ble::ttl_generator_connected()) {
    rig::protocol::MessageHeader header;
    header.opcode = rig::protocol::Opcode::stop_ttl_train;
    header.flags = rig::protocol::require_response |
                   rig::protocol::execute_immediately;
    header.command_id = stop_command;
    header.session_id = current_session;
    std::uint8_t message[rig::protocol::command_max_message_size]{};
    const std::size_t length = rig::protocol::encode_command(
        header, nullptr, 0, message, sizeof(message));
    if (length != 0U) {
      (void)hub_ble::send_to_ttl_generator(rig::transport::Channel::command,
                                          message, length);
    }
  }
}

bool start_sync_test(std::uint32_t command_interval_ms,
                     std::uint32_t width_us) {
  if (atomic_get(&test_enabled) != 0) {
    return true;
  }
  if (atomic_get(&session) == 0 || command_interval_ms < 100U ||
      command_interval_ms > 60'000U) {
    return false;
  }
  test_command_interval_ms = command_interval_ms;
  next_test_command_ms = k_uptime_get();
  telemetry_before_test = hub_nodes::telemetry();
  hub_nodes::set_telemetry(rig::protocol::TelemetryLevel::full);
  atomic_set(&test_enabled, 1);
  if (!start_ttl_train(1000U, width_us, 0)) {
    atomic_clear(&test_enabled);
    hub_nodes::set_telemetry(telemetry_before_test);
    return false;
  }
  std::uint8_t payload[16]{};
  rig::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.boolean(
      static_cast<std::uint16_t>(rig::protocol::Field::test_mode), true);
  publish(rig::protocol::RecordType::test_marker, rig::protocol::critical,
          hub_time::now(), payload, fields.size(), true);
  return true;
}

void stop_sync_test() {
  if (atomic_cas(&test_enabled, 1, 0)) {
    hub_nodes::set_telemetry(telemetry_before_test);
    std::uint8_t payload[16]{};
    rig::protocol::TlvWriter fields(payload, sizeof(payload));
    (void)fields.boolean(
        static_cast<std::uint16_t>(rig::protocol::Field::test_mode), false);
    publish(rig::protocol::RecordType::test_marker, rig::protocol::critical,
            hub_time::now(), payload, fields.size(), true);
  }
  stop_ttl_train();
}

bool sync_test_active() { return atomic_get(&test_enabled) != 0; }

void note_ttl_generator_ttl_status(std::uint32_t sequence,
                               rig::protocol::Status status) {
  const bool accepted = status == rig::protocol::Status::ok ||
                        status == rig::protocol::Status::accepted ||
                        status == rig::protocol::Status::duplicate;
  bool publish_schedule = false;
  std::uint64_t target_ticks = 0;
  std::uint32_t width_us = 0;

  k_mutex_lock(&state_mutex, K_FOREVER);
  PendingPulse *pulse = find_pending(sequence);
  if (pulse != nullptr) {
    if (ttl_in_flight != 0U) {
      --ttl_in_flight;
    }
    if (accepted) {
      target_ticks = pulse->target_ticks;
      width_us = pulse->width_us;
      publish_schedule = true;
      if (ttl_finite && ttl_remaining != 0U) {
        --ttl_remaining;
        if (ttl_remaining == 0U) {
          atomic_clear(&ttl_enabled);
          next_target = 0;
        }
      }
    } else if (atomic_get(&ttl_enabled) != 0) {
      next_target = hub_time::now() + scheduling_lead_ticks * 2ULL;
    }

    // The command response completes Hub's bookkeeping. There is no
    // electrical TTL return capture to wait for.
    pulse->used = false;
  }
  k_mutex_unlock(&state_mutex);

  if (publish_schedule) {
    record_schedule(sequence, target_ticks, width_us);
  }
}

void record_transport_timing(std::uint16_t transfer_id,
                             std::uint32_t command_id, std::uint16_t operation,
                             std::uint64_t receive_ticks,
                             std::uint64_t queued_ticks,
                             std::uint64_t transmit_start_ticks,
                             std::uint64_t transmit_complete_ticks,
                             std::uint16_t fragment_count,
                             std::uint16_t att_mtu) {
  std::uint8_t payload[128]{};
  rig::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.u16(
      static_cast<std::uint16_t>(rig::protocol::Field::related_transfer_id),
      transfer_id);
  (void)fields.u32(
      static_cast<std::uint16_t>(rig::protocol::Field::command_id),
      command_id);
  (void)fields.u16(
      static_cast<std::uint16_t>(rig::protocol::Field::operation), operation);
  (void)fields.u64(
      static_cast<std::uint16_t>(rig::protocol::Field::gateway_receive_ticks),
      receive_ticks);
  (void)fields.u64(
      static_cast<std::uint16_t>(rig::protocol::Field::response_queued_ticks),
      queued_ticks);
  (void)fields.u64(
      static_cast<std::uint16_t>(rig::protocol::Field::transmit_start_ticks),
      transmit_start_ticks);
  (void)fields.u64(static_cast<std::uint16_t>(
                       rig::protocol::Field::transmit_complete_ticks),
                   transmit_complete_ticks);
  (void)fields.u16(
      static_cast<std::uint16_t>(rig::protocol::Field::fragment_count),
      fragment_count);
  (void)fields.u16(static_cast<std::uint16_t>(rig::protocol::Field::att_mtu),
                   att_mtu);
  publish(rig::protocol::RecordType::transport_timing,
          rig::protocol::critical | rig::protocol::actual_time,
          transmit_complete_ticks, payload, fields.size(), true);
}

void note_ttl_generator_ttl(std::uint32_t sequence, std::uint64_t,
                        std::uint64_t) {
  bool publish_schedule = false;
  std::uint64_t target_ticks = 0;
  std::uint32_t width_us = 0;

  k_mutex_lock(&state_mutex, K_FOREVER);
  PendingPulse *pulse = find_pending(sequence);
  if (pulse != nullptr) {
    if (ttl_in_flight != 0U) {
      --ttl_in_flight;
    }
    target_ticks = pulse->target_ticks;
    width_us = pulse->width_us;
    publish_schedule = true;
    if (ttl_finite && ttl_remaining != 0U) {
      --ttl_remaining;
      if (ttl_remaining == 0U) {
        atomic_clear(&ttl_enabled);
        next_target = 0;
      }
    }
    pulse->used = false;
  }
  k_mutex_unlock(&state_mutex);

  if (publish_schedule) {
    record_schedule(sequence, target_ticks, width_us);
  }
}

} // namespace hub_experiment
