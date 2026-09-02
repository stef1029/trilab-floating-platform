#include "experiment.hpp"

#include <array>
#include <cstring>

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>

#include "ble_gateway.hpp"
#include "control.hpp"
#include "fairy_manager.hpp"
#include "fairy_shared/adelie_protocol.hpp"
#include "fairy_shared/fairy_protocol.hpp"
#include "fairy_shared/system_config.hpp"
#include "fairy_shared/tlv.hpp"
#include "galapagos_manager.hpp"
#include "rs485_bus.hpp"
#include "timebase.hpp"

namespace korora_experiment {
namespace {

/* Keep five or fewer pulses queued at 10 Hz (Galapagos holds nine total). */
inline constexpr std::uint64_t scheduling_lead_ticks = 6'400'000ULL;
inline constexpr std::uint64_t minimum_delivery_lead_ticks = 1'600'000ULL;
inline constexpr std::uint32_t default_test_interval_ms = 5000;
inline constexpr std::uint8_t sync_test_valve_address = 0x14U;
inline constexpr std::uint32_t sync_test_valve_duration_ms = 150U;

struct PendingPulse {
  bool used{};
  bool confirmed{};
  std::uint32_t sequence{};
  std::uint32_t width_us{};
  std::uint64_t target_ticks{};
  std::uint64_t generated_ticks{};
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
fairy::protocol::TelemetryLevel telemetry_before_test;

void publish(fairy::protocol::RecordType type, std::uint16_t flags,
             std::uint64_t ticks, const std::uint8_t *payload,
             std::size_t payload_length, bool diagnostic_priority = false) {
  fairy::protocol::RecordHeader header;
  header.type = type;
  header.flags = flags;
  header.record_id = static_cast<std::uint32_t>(atomic_inc(&record_id) + 1);
  header.session_id = static_cast<std::uint32_t>(atomic_get(&session));
  header.timestamp_ticks = ticks;
  header.clock_hz = fairy::config::common_timer_hz;
  std::uint8_t encoded[fairy::protocol::fairy_max_record_size]{};
  const std::size_t length = fairy::protocol::encode_record(
      header, payload, payload_length, encoded, sizeof(encoded));
  if (length != 0U) {
    (void)korora_ble::send_to_adelie(fairy::config::korora_address,
                                     fairy::transport::Channel::fairy, encoded,
                                     length, 0, 0, 0, diagnostic_priority);
  }
}

void record_schedule(std::uint32_t sequence, std::uint64_t target_ticks,
                     std::uint32_t width_us) {
  std::uint8_t payload[40]{};
  fairy::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.u32(static_cast<std::uint16_t>(fairy::protocol::Field::sequence),
                   sequence);
  if (target_ticks != 0U) {
    (void)fields.u64(
        static_cast<std::uint16_t>(fairy::protocol::Field::requested_ticks),
        target_ticks);
  }
  (void)fields.u32(
      static_cast<std::uint16_t>(fairy::protocol::Field::duration_us),
      width_us);
  publish(fairy::protocol::RecordType::ttl_scheduled,
          fairy::protocol::critical | fairy::protocol::scheduled, target_ticks,
          payload, fields.size(), true);
}

void record_capture(std::uint32_t sequence, std::uint64_t captured_ticks,
                    std::uint64_t target_ticks, std::uint64_t generated_ticks) {
  std::uint8_t payload[64]{};
  fairy::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.u32(static_cast<std::uint16_t>(fairy::protocol::Field::sequence),
                   sequence);
  (void)fields.u64(
      static_cast<std::uint16_t>(fairy::protocol::Field::requested_ticks),
      target_ticks);
  (void)fields.u64(
      static_cast<std::uint16_t>(fairy::protocol::Field::actual_ticks),
      captured_ticks);
  publish(fairy::protocol::RecordType::ttl_captured,
          fairy::protocol::critical | fairy::protocol::actual_time |
              fairy::protocol::synchronized,
          captured_ticks, payload, fields.size(), true);

  if (target_ticks == 0U) {
    return;
  }
  const std::int64_t error_ticks =
      captured_ticks >= target_ticks
          ? static_cast<std::int64_t>(captured_ticks - target_ticks)
          : -static_cast<std::int64_t>(target_ticks - captured_ticks);
  fairy::protocol::TlvWriter result(payload, sizeof(payload));
  (void)result.u32(static_cast<std::uint16_t>(fairy::protocol::Field::sequence),
                   sequence);
  (void)result.i64(static_cast<std::uint16_t>(fairy::protocol::Field::value),
                   error_ticks * 125LL / 2LL);
  (void)result.u64(
      static_cast<std::uint16_t>(fairy::protocol::Field::requested_ticks),
      target_ticks);
  (void)result.u64(
      static_cast<std::uint16_t>(fairy::protocol::Field::actual_ticks),
      captured_ticks);
  if (generated_ticks != 0U) {
    (void)result.u64(static_cast<std::uint16_t>(fairy::protocol::Field::detail),
                     generated_ticks);
  }
  publish(fairy::protocol::RecordType::ttl_result,
          fairy::protocol::critical | fairy::protocol::actual_time |
              fairy::protocol::synchronized,
          captured_ticks, payload, result.size(), true);
}

void record_external(const korora_time::Capture &capture) {
  std::uint8_t payload[24]{};
  fairy::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.u32(static_cast<std::uint16_t>(fairy::protocol::Field::sequence),
                   capture.sequence);
  (void)fields.u8(static_cast<std::uint16_t>(fairy::protocol::Field::state), 1);
  publish(fairy::protocol::RecordType::digital_input,
          fairy::protocol::critical | fairy::protocol::actual_time |
              fairy::protocol::synchronized,
          capture.ticks, payload, fields.size(), true);
}

void publish_health(std::uint64_t now) {
  if (now - last_health_ticks < fairy::config::common_timer_hz) {
    return;
  }
  last_health_ticks = now;
  if (korora_fairies::telemetry() ==
      fairy::protocol::TelemetryLevel::critical) {
    return;
  }
  const korora_rs485::Diagnostics rs485 = korora_rs485::diagnostics();
  std::uint8_t payload[144]{};
  fairy::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.u32(
      static_cast<std::uint16_t>(fairy::protocol::Field::uptime_ms),
      static_cast<std::uint32_t>(now / 16'000ULL));
  (void)fields.u32(
      static_cast<std::uint16_t>(fairy::protocol::Field::transport_errors),
      rs485.errors);
  (void)fields.u32(
      static_cast<std::uint16_t>(fairy::protocol::Field::retry_count),
      rs485.retries);
  (void)fields.u32(
      static_cast<std::uint16_t>(fairy::protocol::Field::timeout_count),
      rs485.timeouts);
  (void)fields.u32(static_cast<std::uint16_t>(
                       fairy::protocol::Field::transport_decode_errors),
                   rs485.decode_errors);
  (void)fields.u32(static_cast<std::uint16_t>(
                       fairy::protocol::Field::transport_reassembly_errors),
                   rs485.reassembly_errors);
  (void)fields.u32(static_cast<std::uint16_t>(
                       fairy::protocol::Field::transport_transmit_errors),
                   rs485.transmit_errors);
  (void)fields.u32(
      static_cast<std::uint16_t>(fairy::protocol::Field::ttl_capture_count),
      korora_time::ttl_capture_count());
  (void)fields.u32(
      static_cast<std::uint16_t>(fairy::protocol::Field::ttl_capture_drops),
      korora_time::ttl_capture_drops());
  (void)fields.u32(
      static_cast<std::uint16_t>(fairy::protocol::Field::dropped_records),
      korora_ble::dropped_to_adelie());
  (void)fields.u32(static_cast<std::uint16_t>(fairy::protocol::Field::detail),
                   korora_ble::dropped_to_galapagos());
  (void)fields.i32(static_cast<std::uint16_t>(fairy::protocol::Field::rssi_dbm),
                   korora_ble::galapagos_rssi());
  (void)fields.boolean(
      static_cast<std::uint16_t>(fairy::protocol::Field::state),
      korora_ble::galapagos_connected());
  publish(fairy::protocol::RecordType::link_quality, 0, now, payload,
          fields.size());
}

PendingPulse *allocate_pending(std::uint32_t sequence,
                               std::uint64_t target_ticks,
                               std::uint32_t width_us) {
  for (PendingPulse &pulse : pending) {
    if (!pulse.used) {
      pulse = {true, false, sequence, width_us, target_ticks, 0};
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

PendingPulse *closest_pending(std::uint64_t captured_ticks) {
  PendingPulse *best = nullptr;
  std::uint64_t best_distance = UINT64_MAX;
  for (PendingPulse &pulse : pending) {
    if (!pulse.used) {
      continue;
    }
    const std::uint64_t distance = captured_ticks >= pulse.target_ticks
                                       ? captured_ticks - pulse.target_ticks
                                       : pulse.target_ticks - captured_ticks;
    if (distance < best_distance) {
      best = &pulse;
      best_distance = distance;
    }
  }
  return best_distance <= 8'000'000ULL ? best : nullptr;
}

bool send_ttl_command(std::uint32_t sequence, std::uint64_t target_ticks,
                      std::uint32_t width_us) {
  std::uint64_t galapagos_ticks{};
  std::uint64_t galapagos_now{};
  if (!korora_galapagos::korora_to_local(target_ticks, galapagos_ticks) ||
      !korora_galapagos::korora_to_local(korora_time::now(), galapagos_now) ||
      galapagos_ticks <= galapagos_now + minimum_delivery_lead_ticks) {
    return false;
  }
  galapagos_ticks = (galapagos_ticks / 16ULL) * 16ULL;

  std::uint8_t fields_buffer[24]{};
  fairy::protocol::TlvWriter fields(fields_buffer, sizeof(fields_buffer));
  (void)fields.u32(
      static_cast<std::uint16_t>(fairy::protocol::CommandField::ttl_width_us),
      width_us);
  (void)fields.u32(
      static_cast<std::uint16_t>(fairy::protocol::CommandField::sequence),
      sequence);
  fairy::protocol::MessageHeader header;
  header.opcode = fairy::protocol::Opcode::schedule_ttl;
  header.flags = fairy::protocol::require_response;
  header.command_id = sequence;
  header.session_id = static_cast<std::uint32_t>(atomic_get(&session));
  header.execute_at_ticks = galapagos_ticks;
  header.deadline_ms = 2000;
  std::uint8_t message[fairy::protocol::adelie_max_message_size]{};
  const std::size_t length = fairy::protocol::encode_adelie(
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

  if (korora_ble::send_to_galapagos(fairy::transport::Channel::adelie, message,
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
  fairy::protocol::TlvWriter fields(field_buffer, sizeof(field_buffer));
  (void)fields.u32(
      static_cast<std::uint16_t>(fairy::protocol::CommandField::duration_ms),
      sync_test_valve_duration_ms);

  fairy::protocol::MessageHeader header;
  header.opcode = fairy::protocol::Opcode::actuate_valve;
  header.flags = fairy::protocol::require_response |
                 fairy::protocol::execute_immediately |
                 fairy::protocol::safety_authorized;
  header.command_id =
      static_cast<std::uint32_t>(atomic_inc(&sequence_counter) + 1);
  header.session_id = static_cast<std::uint32_t>(atomic_get(&session));

  std::uint8_t message[fairy::protocol::adelie_max_message_size]{};
  const std::size_t length = fairy::protocol::encode_adelie(
      header, field_buffer, fields.size(), message, sizeof(message));
  if (length != 0U) {
    (void)korora_fairies::queue_command(sync_test_valve_address, message,
                                        length, 0);
  }
}

void process_capture(const korora_time::Capture &capture) {
  if (capture.kind == korora_time::CaptureKind::external_event) {
    record_external(capture);
    return;
  }
  k_mutex_lock(&state_mutex, K_FOREVER);
  PendingPulse *match = closest_pending(capture.ticks);
  if (match != nullptr) {
    const bool implicit_acceptance = !match->confirmed;
    if (implicit_acceptance) {
      if (ttl_in_flight != 0U) {
        --ttl_in_flight;
      }
      if (ttl_finite && ttl_remaining != 0U) {
        --ttl_remaining;
        if (ttl_remaining == 0U) {
          atomic_clear(&ttl_enabled);
          next_target = 0;
        }
      }
    }
    const PendingPulse copy = *match;
    match->used = false;
    k_mutex_unlock(&state_mutex);
    if (implicit_acceptance) {
      record_schedule(copy.sequence, copy.target_ticks, copy.width_us);
    }
    record_capture(copy.sequence, capture.ticks, copy.target_ticks,
                   copy.generated_ticks);
  } else {
    k_mutex_unlock(&state_mutex);
    record_capture(capture.sequence, capture.ticks, 0, 0);
  }
}

void worker(void *, void *, void *) {
  k_sem_take(&start_sem, K_FOREVER);
  while (true) {
    korora_time::Capture capture;
    while (korora_time::pop_capture(capture)) {
      process_capture(capture);
    }

    const std::uint64_t now = korora_time::now();
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
        next_target = korora_time::now() + scheduling_lead_ticks * 2ULL;
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
  telemetry_before_test = fairy::protocol::TelemetryLevel::standard;
  for (PendingPulse &pulse : pending) {
    pulse = {};
  }
  k_sem_give(&start_sem);
}

void adelie_connected() {
  std::uint8_t payload[64]{};
  fairy::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.string(
      static_cast<std::uint16_t>(fairy::protocol::Field::board_kind),
      "nrf52840dk");
  (void)fields.string(
      static_cast<std::uint16_t>(fairy::protocol::Field::firmware_version),
      "3.0.0");
  publish(fairy::protocol::RecordType::boot, fairy::protocol::first_after_reset,
          korora_time::now(), payload, fields.size());
}

void publish_fault(const char *reason) {
  std::uint8_t payload[96]{};
  fairy::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.string(
      static_cast<std::uint16_t>(fairy::protocol::Field::reason), reason);
  publish(fairy::protocol::RecordType::fault,
          fairy::protocol::critical | fairy::protocol::loss_latched,
          korora_time::now(), payload, fields.size(), true);
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
  next_target = korora_time::now() + scheduling_lead_ticks * 2ULL;
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
  if (current_session != 0U && korora_ble::galapagos_connected()) {
    fairy::protocol::MessageHeader header;
    header.opcode = fairy::protocol::Opcode::stop_ttl_train;
    header.flags = fairy::protocol::require_response |
                   fairy::protocol::execute_immediately;
    header.command_id = stop_command;
    header.session_id = current_session;
    std::uint8_t message[fairy::protocol::adelie_max_message_size]{};
    const std::size_t length = fairy::protocol::encode_adelie(
        header, nullptr, 0, message, sizeof(message));
    if (length != 0U) {
      (void)korora_ble::send_to_galapagos(fairy::transport::Channel::adelie,
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
  telemetry_before_test = korora_fairies::telemetry();
  korora_fairies::set_telemetry(fairy::protocol::TelemetryLevel::full);
  atomic_set(&test_enabled, 1);
  if (!start_ttl_train(1000U, width_us, 0)) {
    atomic_clear(&test_enabled);
    korora_fairies::set_telemetry(telemetry_before_test);
    return false;
  }
  std::uint8_t payload[16]{};
  fairy::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.boolean(
      static_cast<std::uint16_t>(fairy::protocol::Field::test_mode), true);
  publish(fairy::protocol::RecordType::test_marker, fairy::protocol::critical,
          korora_time::now(), payload, fields.size(), true);
  return true;
}

void stop_sync_test() {
  if (atomic_cas(&test_enabled, 1, 0)) {
    korora_fairies::set_telemetry(telemetry_before_test);
    std::uint8_t payload[16]{};
    fairy::protocol::TlvWriter fields(payload, sizeof(payload));
    (void)fields.boolean(
        static_cast<std::uint16_t>(fairy::protocol::Field::test_mode), false);
    publish(fairy::protocol::RecordType::test_marker, fairy::protocol::critical,
            korora_time::now(), payload, fields.size(), true);
  }
  stop_ttl_train();
}

bool sync_test_active() { return atomic_get(&test_enabled) != 0; }

void note_galapagos_ttl_status(std::uint32_t sequence,
                               fairy::protocol::Status status) {
  const bool accepted = status == fairy::protocol::Status::ok ||
                        status == fairy::protocol::Status::accepted ||
                        status == fairy::protocol::Status::duplicate;
  bool publish_schedule = false;
  std::uint64_t target_ticks = 0;
  std::uint32_t width_us = 0;

  k_mutex_lock(&state_mutex, K_FOREVER);
  PendingPulse *pulse = find_pending(sequence);
  if (pulse != nullptr) {
    if (!pulse->confirmed && ttl_in_flight != 0U) {
      --ttl_in_flight;
    }
    if (accepted) {
      if (!pulse->confirmed) {
        pulse->confirmed = true;
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
      }
    } else {
      pulse->used = false;
      if (atomic_get(&ttl_enabled) != 0) {
        next_target = korora_time::now() + scheduling_lead_ticks * 2ULL;
      }
    }
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
  fairy::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.u16(
      static_cast<std::uint16_t>(fairy::protocol::Field::related_transfer_id),
      transfer_id);
  (void)fields.u32(
      static_cast<std::uint16_t>(fairy::protocol::Field::command_id),
      command_id);
  (void)fields.u16(
      static_cast<std::uint16_t>(fairy::protocol::Field::operation), operation);
  (void)fields.u64(
      static_cast<std::uint16_t>(fairy::protocol::Field::gateway_receive_ticks),
      receive_ticks);
  (void)fields.u64(
      static_cast<std::uint16_t>(fairy::protocol::Field::response_queued_ticks),
      queued_ticks);
  (void)fields.u64(
      static_cast<std::uint16_t>(fairy::protocol::Field::transmit_start_ticks),
      transmit_start_ticks);
  (void)fields.u64(static_cast<std::uint16_t>(
                       fairy::protocol::Field::transmit_complete_ticks),
                   transmit_complete_ticks);
  (void)fields.u16(
      static_cast<std::uint16_t>(fairy::protocol::Field::fragment_count),
      fragment_count);
  (void)fields.u16(static_cast<std::uint16_t>(fairy::protocol::Field::att_mtu),
                   att_mtu);
  publish(fairy::protocol::RecordType::transport_timing,
          fairy::protocol::critical | fairy::protocol::actual_time,
          transmit_complete_ticks, payload, fields.size(), true);
}

void note_galapagos_ttl(std::uint32_t sequence, std::uint64_t,
                        std::uint64_t korora_actual_ticks) {
  k_mutex_lock(&state_mutex, K_FOREVER);
  for (PendingPulse &pulse : pending) {
    if (pulse.used && pulse.sequence == sequence) {
      pulse.generated_ticks = korora_actual_ticks;
      break;
    }
  }
  k_mutex_unlock(&state_mutex);
}

} // namespace korora_experiment
