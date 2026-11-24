#include "application.hpp"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "rig_protocol/command_protocol.hpp"
#include "rig_protocol/record_protocol.hpp"
#include "rig_protocol/tlv.hpp"
#include "hardware.hpp"
#include "record_stream.hpp"

namespace ttl_generator_application {
namespace {

atomic_t active_session;
std::uint32_t last_command;
bool link_connected;
K_SEM_DEFINE(start_sem, 0, 1);

std::uint64_t extend_controller_anchor_us(std::uint64_t raw_anchor_us) {
  /*
   * The SoftDevice anchor report contains a wrapping 32-bit microsecond
   * timestamp. Extend it around the local 64-bit GRTC time before feeding the
   * clock model; otherwise every ~71.6 minutes the model jumps into the past
   * and scheduled TTL timestamps are rejected by the hardware.
   */
  constexpr std::uint64_t wrap = 1ULL << 32U;
  constexpr std::uint64_t half_wrap = wrap / 2ULL;
  const std::uint64_t near_us = ttl_generator_hardware::now_ticks() / 16ULL;
  std::uint64_t extended =
      (near_us & ~(wrap - 1ULL)) | (raw_anchor_us & (wrap - 1ULL));
  if (extended + half_wrap < near_us) {
    extended += wrap;
  } else if (extended > near_us + half_wrap && extended >= wrap) {
    extended -= wrap;
  }
  return extended;
}

std::uint32_t session_value() {
  return static_cast<std::uint32_t>(atomic_get(&active_session));
}

void publish_boot() {
  std::uint8_t payload[64]{};
  rig::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.string(
      static_cast<std::uint16_t>(rig::protocol::Field::board_kind),
      "nrf54l15dk");
  (void)fields.string(
      static_cast<std::uint16_t>(rig::protocol::Field::firmware_version),
      "3.0.0");
  (void)ttl_generator_stream::publish_record(
      rig::protocol::RecordType::boot, rig::protocol::first_after_reset,
      ttl_generator_hardware::now_ticks(), 0, payload, fields.size());
}

bool command_u32(const rig::protocol::CommandMessageView &message,
                 rig::protocol::CommandField wanted, std::uint32_t &value) {
  rig::protocol::TlvReader reader(message.payload,
                                    message.header.payload_length);
  rig::protocol::FieldView field;
  while (reader.next(field)) {
    if (field.tag == static_cast<std::uint16_t>(wanted)) {
      return rig::protocol::TlvReader::as_u32(field, value);
    }
  }
  return false;
}

void publish_result(const rig::protocol::CommandMessageView &command,
                    rig::protocol::Status status, std::uint64_t ticks) {
  std::uint8_t payload[64]{};
  rig::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.u32(
      static_cast<std::uint16_t>(rig::protocol::Field::command_id),
      command.header.command_id);
  (void)fields.u16(
      static_cast<std::uint16_t>(rig::protocol::Field::operation),
      static_cast<std::uint16_t>(command.header.opcode));
  (void)fields.u8(static_cast<std::uint16_t>(rig::protocol::Field::status),
                  static_cast<std::uint8_t>(status));
  (void)fields.u64(
      static_cast<std::uint16_t>(rig::protocol::Field::requested_ticks),
      command.header.execute_at_ticks);
  (void)fields.u64(
      static_cast<std::uint16_t>(rig::protocol::Field::actual_ticks), ticks);
  (void)ttl_generator_stream::publish_record(
      rig::protocol::RecordType::command_result,
      rig::protocol::critical | rig::protocol::actual_time, ticks,
      session_value(), payload, fields.size());
}

void health_thread(void *, void *, void *) {
  k_sem_take(&start_sem, K_FOREVER);
  while (true) {
    k_sleep(K_SECONDS(1));
    const std::uint64_t ticks = ttl_generator_hardware::now_ticks();
    std::uint8_t payload[80]{};
    rig::protocol::TlvWriter fields(payload, sizeof(payload));
    (void)fields.u32(
        static_cast<std::uint16_t>(rig::protocol::Field::uptime_ms),
        static_cast<std::uint32_t>(ticks / 16'000ULL));
    (void)fields.u32(
        static_cast<std::uint16_t>(rig::protocol::Field::dropped_records),
        ttl_generator_stream::dropped());
    (void)fields.u32(
        static_cast<std::uint16_t>(rig::protocol::Field::transport_errors),
        ttl_generator_stream::transport_errors());
    (void)fields.boolean(
        static_cast<std::uint16_t>(rig::protocol::Field::state),
        ttl_generator_hardware::ttl_armed());
    (void)ttl_generator_stream::publish_record(rig::protocol::RecordType::health,
                                           0, ticks, session_value(), payload,
                                           fields.size());
  }
}

K_THREAD_DEFINE(health_thread_id, 1536, health_thread, nullptr, nullptr,
                nullptr, 9, 0, 0);

} // namespace

void initialize() {
  atomic_clear(&active_session);
  last_command = 0;
  link_connected = false;
  k_sem_give(&start_sem);
}

std::size_t handle_command(const std::uint8_t *message, std::size_t length,
                           std::uint8_t *response, std::size_t capacity) {
  rig::protocol::CommandMessageView command;
  if (!rig::protocol::decode_command(message, length, command) ||
      command.header.kind != rig::protocol::MessageKind::command) {
    return 0;
  }

  rig::protocol::Status status = rig::protocol::Status::ok;
  const std::uint64_t now = ttl_generator_hardware::now_ticks();
  const std::uint32_t current_session = session_value();
  if (command.header.command_id == last_command &&
      command.header.command_id != 0U) {
    status = rig::protocol::Status::duplicate;
  } else {
    switch (command.header.opcode) {
    case rig::protocol::Opcode::ping:
    case rig::protocol::Opcode::get_health:
      break;

    case rig::protocol::Opcode::start_session:
      if (command.header.session_id == 0U) {
        status = rig::protocol::Status::invalid_parameter;
      } else if (current_session != 0U &&
                 current_session != command.header.session_id) {
        status = rig::protocol::Status::busy;
      } else {
        atomic_set(&active_session,
                   static_cast<atomic_val_t>(command.header.session_id));
        ttl_generator_hardware::set_session(command.header.session_id);
      }
      break;

    case rig::protocol::Opcode::stop_session:
      if (current_session != 0U &&
          command.header.session_id != current_session) {
        status = rig::protocol::Status::session_mismatch;
      } else {
        atomic_clear(&active_session);
        ttl_generator_hardware::set_session(0);
        ttl_generator_hardware::force_ttl_low();
      }
      break;

    case rig::protocol::Opcode::schedule_ttl: {
      if (current_session == 0U ||
          command.header.session_id != current_session) {
        status = rig::protocol::Status::session_mismatch;
        break;
      }
      std::uint32_t width{};
      std::uint32_t sequence = command.header.command_id;
      if (!command_u32(command, rig::protocol::CommandField::ttl_width_us,
                       width)) {
        status = rig::protocol::Status::invalid_parameter;
        break;
      }
      (void)command_u32(command, rig::protocol::CommandField::sequence,
                        sequence);
      status = ttl_generator_hardware::schedule_ttl(
          sequence, command.header.execute_at_ticks, width);
      break;
    }

    case rig::protocol::Opcode::stop_ttl_train:
      ttl_generator_hardware::force_ttl_low();
      break;

    default:
      status = rig::protocol::Status::unsupported;
      break;
    }
    last_command = command.header.command_id;
    publish_result(command, status, now);
  }

  rig::protocol::MessageHeader header = command.header;
  header.kind = rig::protocol::MessageKind::response;
  header.status = status;
  header.flags = 0;
  return rig::protocol::encode_command(header, nullptr, 0, response, capacity);
}

void anchor_observation(std::uint16_t event_counter, std::uint64_t anchor_us) {
  const std::uint32_t current_session = session_value();
  std::uint8_t payload[24]{};
  rig::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.u32(static_cast<std::uint16_t>(rig::protocol::Field::sequence),
                   event_counter);
  (void)ttl_generator_stream::publish_record(
      rig::protocol::RecordType::sync_observation,
      current_session == 0U ? 0U : rig::protocol::synchronized,
      extend_controller_anchor_us(anchor_us) * 16ULL, current_session, payload,
      fields.size());
}

void connected() {
  link_connected = true;
  publish_boot();
}

void disconnected() {
  link_connected = false;
  atomic_clear(&active_session);
  ttl_generator_hardware::set_session(0);
  ttl_generator_hardware::force_ttl_low();
}

} // namespace ttl_generator_application
