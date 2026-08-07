#include "application.hpp"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "fairy_shared/adelie_protocol.hpp"
#include "fairy_shared/fairy_protocol.hpp"
#include "fairy_shared/tlv.hpp"
#include "hardware.hpp"
#include "record_stream.hpp"

namespace galapagos_application {
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
  const std::uint64_t near_us = galapagos_hardware::now_ticks() / 16ULL;
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
  fairy::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.string(
      static_cast<std::uint16_t>(fairy::protocol::Field::board_kind),
      "nrf54l15dk");
  (void)fields.string(
      static_cast<std::uint16_t>(fairy::protocol::Field::firmware_version),
      "3.0.0");
  (void)galapagos_stream::publish_record(
      fairy::protocol::RecordType::boot, fairy::protocol::first_after_reset,
      galapagos_hardware::now_ticks(), 0, payload, fields.size());
}

bool command_u32(const fairy::protocol::AdelieMessageView &message,
                 fairy::protocol::CommandField wanted, std::uint32_t &value) {
  fairy::protocol::TlvReader reader(message.payload,
                                    message.header.payload_length);
  fairy::protocol::FieldView field;
  while (reader.next(field)) {
    if (field.tag == static_cast<std::uint16_t>(wanted)) {
      return fairy::protocol::TlvReader::as_u32(field, value);
    }
  }
  return false;
}

void publish_result(const fairy::protocol::AdelieMessageView &command,
                    fairy::protocol::Status status, std::uint64_t ticks) {
  std::uint8_t payload[64]{};
  fairy::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.u32(
      static_cast<std::uint16_t>(fairy::protocol::Field::command_id),
      command.header.command_id);
  (void)fields.u16(
      static_cast<std::uint16_t>(fairy::protocol::Field::operation),
      static_cast<std::uint16_t>(command.header.opcode));
  (void)fields.u8(static_cast<std::uint16_t>(fairy::protocol::Field::status),
                  static_cast<std::uint8_t>(status));
  (void)fields.u64(
      static_cast<std::uint16_t>(fairy::protocol::Field::requested_ticks),
      command.header.execute_at_ticks);
  (void)fields.u64(
      static_cast<std::uint16_t>(fairy::protocol::Field::actual_ticks), ticks);
  (void)galapagos_stream::publish_record(
      fairy::protocol::RecordType::command_result,
      fairy::protocol::critical | fairy::protocol::actual_time, ticks,
      session_value(), payload, fields.size());
}

void health_thread(void *, void *, void *) {
  k_sem_take(&start_sem, K_FOREVER);
  while (true) {
    k_sleep(K_SECONDS(1));
    const std::uint64_t ticks = galapagos_hardware::now_ticks();
    std::uint8_t payload[80]{};
    fairy::protocol::TlvWriter fields(payload, sizeof(payload));
    (void)fields.u32(
        static_cast<std::uint16_t>(fairy::protocol::Field::uptime_ms),
        static_cast<std::uint32_t>(ticks / 16'000ULL));
    (void)fields.u32(
        static_cast<std::uint16_t>(fairy::protocol::Field::dropped_records),
        galapagos_stream::dropped());
    (void)fields.u32(
        static_cast<std::uint16_t>(fairy::protocol::Field::transport_errors),
        galapagos_stream::transport_errors());
    (void)fields.boolean(
        static_cast<std::uint16_t>(fairy::protocol::Field::state),
        galapagos_hardware::ttl_armed());
    (void)galapagos_stream::publish_record(fairy::protocol::RecordType::health,
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
  fairy::protocol::AdelieMessageView command;
  if (!fairy::protocol::decode_adelie(message, length, command) ||
      command.header.kind != fairy::protocol::MessageKind::command) {
    return 0;
  }

  fairy::protocol::Status status = fairy::protocol::Status::ok;
  const std::uint64_t now = galapagos_hardware::now_ticks();
  const std::uint32_t current_session = session_value();
  if (command.header.command_id == last_command &&
      command.header.command_id != 0U) {
    status = fairy::protocol::Status::duplicate;
  } else {
    switch (command.header.opcode) {
    case fairy::protocol::Opcode::ping:
    case fairy::protocol::Opcode::get_health:
      break;

    case fairy::protocol::Opcode::start_session:
      if (command.header.session_id == 0U) {
        status = fairy::protocol::Status::invalid_parameter;
      } else if (current_session != 0U &&
                 current_session != command.header.session_id) {
        status = fairy::protocol::Status::busy;
      } else {
        atomic_set(&active_session,
                   static_cast<atomic_val_t>(command.header.session_id));
        galapagos_hardware::set_session(command.header.session_id);
      }
      break;

    case fairy::protocol::Opcode::stop_session:
      if (current_session != 0U &&
          command.header.session_id != current_session) {
        status = fairy::protocol::Status::session_mismatch;
      } else {
        atomic_clear(&active_session);
        galapagos_hardware::set_session(0);
        galapagos_hardware::force_ttl_low();
      }
      break;

    case fairy::protocol::Opcode::schedule_ttl: {
      if (current_session == 0U ||
          command.header.session_id != current_session) {
        status = fairy::protocol::Status::session_mismatch;
        break;
      }
      std::uint32_t width{};
      std::uint32_t sequence = command.header.command_id;
      if (!command_u32(command, fairy::protocol::CommandField::ttl_width_us,
                       width)) {
        status = fairy::protocol::Status::invalid_parameter;
        break;
      }
      (void)command_u32(command, fairy::protocol::CommandField::sequence,
                        sequence);
      status = galapagos_hardware::schedule_ttl(
          sequence, command.header.execute_at_ticks, width);
      break;
    }

    case fairy::protocol::Opcode::stop_ttl_train:
      galapagos_hardware::force_ttl_low();
      break;

    default:
      status = fairy::protocol::Status::unsupported;
      break;
    }
    last_command = command.header.command_id;
    publish_result(command, status, now);
  }

  fairy::protocol::MessageHeader header = command.header;
  header.kind = fairy::protocol::MessageKind::response;
  header.status = status;
  header.flags = 0;
  return fairy::protocol::encode_adelie(header, nullptr, 0, response, capacity);
}

void anchor_observation(std::uint16_t event_counter, std::uint64_t anchor_us) {
  const std::uint32_t current_session = session_value();
  std::uint8_t payload[24]{};
  fairy::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.u32(static_cast<std::uint16_t>(fairy::protocol::Field::sequence),
                   event_counter);
  (void)galapagos_stream::publish_record(
      fairy::protocol::RecordType::sync_observation,
      current_session == 0U ? 0U : fairy::protocol::synchronized,
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
  galapagos_hardware::set_session(0);
  galapagos_hardware::force_ttl_low();
}

} // namespace galapagos_application
