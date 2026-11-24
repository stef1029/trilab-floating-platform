#include "ttl_generator_manager.hpp"

#include <array>
#include <cstring>

#include <zephyr/kernel.h>

#include "ble_gateway.hpp"
#include "controller_clock.hpp"
#include "experiment.hpp"
#include "node_manager.hpp"
#include "rig_protocol/command_protocol.hpp"
#include "rig_protocol/record_protocol.hpp"
#include "rig_protocol/system_config.hpp"
#include "rig_protocol/tlv.hpp"
#include "timebase.hpp"

namespace hub_ttl_generator {
namespace {

struct Anchor {
  bool valid{};
  std::uint16_t event_counter{};
  std::uint64_t hub_ticks{};
};

std::array<Anchor, 256> anchors;
std::size_t next_anchor;
rig::time::AffineClockModel<16> model;
k_mutex state_mutex;
std::uint64_t last_quality_ticks;
std::uint64_t last_model_update_ticks;
std::uint32_t quality_record_id;
bool link_active;

bool field_u32(const rig::protocol::RecordView &record,
               rig::protocol::Field wanted, std::uint32_t &value) {
  rig::protocol::TlvReader reader(record.payload,
                                    record.header.payload_length);
  rig::protocol::FieldView field;
  while (reader.next(field)) {
    if (field.tag == static_cast<std::uint16_t>(wanted)) {
      return rig::protocol::TlvReader::as_u32(field, value);
    }
  }
  return false;
}

bool field_u64(const rig::protocol::RecordView &record,
               rig::protocol::Field wanted, std::uint64_t &value) {
  rig::protocol::TlvReader reader(record.payload,
                                    record.header.payload_length);
  rig::protocol::FieldView field;
  while (reader.next(field)) {
    if (field.tag == static_cast<std::uint16_t>(wanted)) {
      return rig::protocol::TlvReader::as_u64(field, value);
    }
  }
  return false;
}

bool find_anchor(std::uint16_t event_counter, std::uint64_t &ticks) {
  for (const Anchor &anchor : anchors) {
    if (anchor.valid && anchor.event_counter == event_counter) {
      ticks = anchor.hub_ticks;
      return true;
    }
  }
  return false;
}

void send_record(rig::protocol::RecordType type, std::uint16_t flags,
                 std::uint32_t record_id, std::uint32_t session_id,
                 std::uint64_t timestamp_ticks, const std::uint8_t *payload,
                 std::size_t payload_length, bool diagnostic_priority) {
  rig::protocol::RecordHeader header;
  header.type = type;
  header.flags = flags;
  header.record_id = record_id;
  header.session_id = session_id;
  header.timestamp_ticks = timestamp_ticks;
  header.clock_hz = rig::config::common_timer_hz;
  std::uint8_t encoded[rig::protocol::max_record_size]{};
  const std::size_t length = rig::protocol::encode_record(
      header, payload, payload_length, encoded, sizeof(encoded));
  if (length != 0U) {
    (void)hub_ble::send_to_host(rig::config::ttl_generator_address,
                                     rig::transport::Channel::record, encoded,
                                     length, 0, 0, 0, diagnostic_priority);
  }
}

void publish_quality(std::uint64_t now) {
  if (hub_nodes::telemetry() ==
      rig::protocol::TelemetryLevel::critical) {
    return;
  }
  k_mutex_lock(&state_mutex, K_FOREVER);
  if (now - last_quality_ticks < rig::config::common_timer_hz) {
    k_mutex_unlock(&state_mutex);
    return;
  }
  last_quality_ticks = now;
  const rig::time::ClockQuality current = model.quality();
  k_mutex_unlock(&state_mutex);
  const rig::time::ClockQuality bridge = hub_controller_clock::quality();
  std::uint8_t payload[96]{};
  rig::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.i64(static_cast<std::uint16_t>(rig::protocol::Field::rms_ns),
                   static_cast<std::int64_t>(current.rms_ticks * 62.5));
  (void)fields.i64(static_cast<std::uint16_t>(rig::protocol::Field::skew_ppb),
                   current.skew_ppb);
  (void)fields.u8(
      static_cast<std::uint16_t>(rig::protocol::Field::model_points),
      static_cast<std::uint8_t>(current.points));
  (void)fields.u32(
      static_cast<std::uint16_t>(rig::protocol::Field::model_generation),
      current.generation);
  (void)fields.u8(static_cast<std::uint16_t>(rig::protocol::Field::state),
                  bridge.valid ? 1U : 0U);
  (void)fields.i64(
      static_cast<std::uint16_t>(rig::protocol::Field::interval_error_ppb),
      bridge.skew_ppb);
  send_record(rig::protocol::RecordType::sync_quality,
              current.valid ? rig::protocol::synchronized : 0U,
              ++quality_record_id, 0, now, payload, fields.size(), false);
}

void process_sync(const rig::protocol::RecordView &record) {
  std::uint32_t raw_counter{};
  if (!field_u32(record, rig::protocol::Field::sequence, raw_counter)) {
    return;
  }
  std::uint64_t reference{};
  k_mutex_lock(&state_mutex, K_FOREVER);
  const bool found =
      find_anchor(static_cast<std::uint16_t>(raw_counter), reference);
  if (found && model.add(record.header.timestamp_ticks, reference)) {
    last_model_update_ticks = reference;
  }
  k_mutex_unlock(&state_mutex);

  if (found &&
      hub_nodes::telemetry() == rig::protocol::TelemetryLevel::full) {
    std::uint8_t payload[48]{};
    rig::protocol::TlvWriter fields(payload, sizeof(payload));
    (void)fields.u32(
        static_cast<std::uint16_t>(rig::protocol::Field::sequence),
        raw_counter);
    (void)fields.u64(
        static_cast<std::uint16_t>(rig::protocol::Field::requested_ticks),
        reference);
    (void)fields.u64(
        static_cast<std::uint16_t>(rig::protocol::Field::actual_ticks),
        record.header.timestamp_ticks);
    send_record(rig::protocol::RecordType::clock_pair,
                rig::protocol::synchronized, record.header.record_id,
                record.header.session_id, reference, payload, fields.size(),
                false);
  }
}

void forward_record(const rig::protocol::RecordView &record,
                    const std::uint8_t *raw, std::size_t raw_length) {
  if (!rig::protocol::record_visible_at(record.header.type,
                                          hub_nodes::telemetry())) {
    return;
  }

  const bool timed_event =
      record.header.type == rig::protocol::RecordType::digital_input ||
      record.header.type == rig::protocol::RecordType::ttl_generated ||
      record.header.type == rig::protocol::RecordType::command_result;
  if (!timed_event) {
    (void)hub_ble::send_to_host(rig::config::ttl_generator_address,
                                     rig::transport::Channel::record, raw,
                                     raw_length);
    return;
  }

  std::uint8_t payload[rig::protocol::record_max_payload]{};
  std::size_t payload_length = record.header.payload_length;
  std::memcpy(payload, record.payload, payload_length);
  std::uint64_t converted{};
  std::uint16_t flags = record.header.flags;
  k_mutex_lock(&state_mutex, K_FOREVER);
  const bool synchronized =
      model.predict(record.header.timestamp_ticks, converted);
  k_mutex_unlock(&state_mutex);
  if (synchronized) {
    rig::protocol::TlvWriter extra(payload + payload_length,
                                     sizeof(payload) - payload_length);
    if (extra.u64(
            static_cast<std::uint16_t>(rig::protocol::Field::reference_ticks),
            converted)) {
      payload_length += extra.size();
      flags |= rig::protocol::synchronized;
    }
  }
  send_record(record.header.type, flags, record.header.record_id,
              record.header.session_id, record.header.timestamp_ticks, payload,
              payload_length, true);

  if (record.header.type == rig::protocol::RecordType::ttl_generated &&
      synchronized) {
    std::uint32_t sequence{};
    std::uint64_t local_actual = record.header.timestamp_ticks;
    (void)field_u32(record, rig::protocol::Field::sequence, sequence);
    (void)field_u64(record, rig::protocol::Field::actual_ticks, local_actual);
    hub_experiment::note_ttl_generator_ttl(sequence, local_actual, converted);
  }
}

} // namespace

void initialize() {
  k_mutex_init(&state_mutex);
  k_mutex_lock(&state_mutex, K_FOREVER);
  model.reset();
  model.set_admission(3200.0, 6.0, 3);
  for (Anchor &anchor : anchors) {
    anchor = {};
  }
  next_anchor = 0;
  last_quality_ticks = 0;
  last_model_update_ticks = 0;
  quality_record_id = 0;
  link_active = false;
  k_mutex_unlock(&state_mutex);
}

void connected() {
  k_mutex_lock(&state_mutex, K_FOREVER);
  link_active = true;
  model.reset();
  last_model_update_ticks = 0;
  for (Anchor &anchor : anchors) {
    anchor.valid = false;
  }
  k_mutex_unlock(&state_mutex);
}

void disconnected() {
  k_mutex_lock(&state_mutex, K_FOREVER);
  link_active = false;
  model.reset();
  last_model_update_ticks = 0;
  for (Anchor &anchor : anchors) {
    anchor.valid = false;
  }
  k_mutex_unlock(&state_mutex);
}

void central_anchor(std::uint16_t event_counter,
                    std::uint64_t controller_ticks) {
  std::uint64_t converted{};
  if (!hub_controller_clock::to_hub(controller_ticks, converted)) {
    return;
  }
  k_mutex_lock(&state_mutex, K_FOREVER);
  anchors[next_anchor] = {true, event_counter, converted};
  next_anchor = (next_anchor + 1U) % anchors.size();
  k_mutex_unlock(&state_mutex);
}

void receive(const rig::transport::MessageView &message) {
  if (message.channel == rig::transport::Channel::command) {
    rig::protocol::CommandMessageView response;
    if (rig::protocol::decode_command(message.payload, message.payload_length,
                                       response) &&
        response.header.kind == rig::protocol::MessageKind::response &&
        response.header.opcode == rig::protocol::Opcode::schedule_ttl) {
      hub_experiment::note_ttl_generator_ttl_status(response.header.command_id,
                                                   response.header.status);
    }
    (void)hub_ble::send_to_host(rig::config::ttl_generator_address,
                                     rig::transport::Channel::command,
                                     message.payload, message.payload_length,
                                     message.transfer_id, message.flags);
    return;
  }
  if (message.channel != rig::transport::Channel::record) {
    return;
  }
  rig::protocol::RecordView record;
  if (!rig::protocol::decode_record(message.payload, message.payload_length,
                                      record)) {
    return;
  }
  if (record.header.type == rig::protocol::RecordType::sync_observation) {
    process_sync(record);
  }
  forward_record(record, message.payload, message.payload_length);
  publish_quality(hub_time::now());
}

bool hub_to_local(std::uint64_t hub_ticks, std::uint64_t &local_ticks) {
  k_mutex_lock(&state_mutex, K_FOREVER);
  const std::uint64_t now = hub_time::now();
  const bool fresh =
      last_model_update_ticks != 0U && now >= last_model_update_ticks &&
      now - last_model_update_ticks < 3ULL * rig::config::common_timer_hz;
  const bool result =
      link_active && fresh && model.inverse(hub_ticks, local_ticks);
  k_mutex_unlock(&state_mutex);
  return result;
}

bool local_to_hub(std::uint64_t local_ticks, std::uint64_t &hub_ticks) {
  k_mutex_lock(&state_mutex, K_FOREVER);
  const std::uint64_t now = hub_time::now();
  const bool fresh =
      last_model_update_ticks != 0U && now >= last_model_update_ticks &&
      now - last_model_update_ticks < 3ULL * rig::config::common_timer_hz;
  const bool result =
      link_active && fresh && model.predict(local_ticks, hub_ticks);
  k_mutex_unlock(&state_mutex);
  return result;
}

rig::time::ClockQuality quality() {
  k_mutex_lock(&state_mutex, K_FOREVER);
  const auto result = model.quality();
  k_mutex_unlock(&state_mutex);
  return result;
}

} // namespace hub_ttl_generator
