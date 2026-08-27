#include "control.hpp"

#include <array>
#include <cstring>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "ble_gateway.hpp"
#include "experiment.hpp"
#include "fairy_manager.hpp"
#include "fairy_shared/adelie_protocol.hpp"
#include "fairy_shared/fairy_protocol.hpp"
#include "fairy_shared/system_config.hpp"
#include "fairy_shared/tlv.hpp"
#include "galapagos_manager.hpp"
#include "local_sensors.hpp"
#include "rs485_bus.hpp"
#include "timebase.hpp"

namespace korora_control {
namespace {

inline constexpr std::uint32_t diagnostic_session = 0x53594E43U;

atomic_t session_id;
bool diagnostic_session_active;
k_mutex control_mutex;

bool command_u8(const fairy::protocol::AdelieMessageView &message,
                fairy::protocol::CommandField wanted, std::uint8_t &value) {
  fairy::protocol::TlvReader reader(message.payload,
                                    message.header.payload_length);
  fairy::protocol::FieldView field;
  while (reader.next(field)) {
    if (field.tag == static_cast<std::uint16_t>(wanted)) {
      return fairy::protocol::TlvReader::as_u8(field, value);
    }
  }
  return false;
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

bool command_bytes(const fairy::protocol::AdelieMessageView &message,
                   fairy::protocol::CommandField wanted,
                   const std::uint8_t *&value, std::size_t &length) {
  fairy::protocol::TlvReader reader(message.payload,
                                    message.header.payload_length);
  fairy::protocol::FieldView field;
  while (reader.next(field)) {
    if (field.tag == static_cast<std::uint16_t>(wanted) &&
        field.type == fairy::protocol::ValueType::bytes) {
      value = field.value;
      length = field.length;
      return true;
    }
  }
  return false;
}

void send_response(const fairy::protocol::AdelieMessageView &command,
                   fairy::protocol::Status status, std::uint16_t transfer_id,
                   const std::uint8_t *payload = nullptr,
                   std::size_t payload_length = 0,
                   std::uint64_t receive_ticks = 0) {
  fairy::protocol::MessageHeader header = command.header;
  header.kind = fairy::protocol::MessageKind::response;
  header.status = status;
  header.flags = 0;
  header.execute_at_ticks = 0;
  header.payload_length = 0;
  std::uint8_t response[fairy::protocol::adelie_max_message_size]{};
  const std::size_t length = fairy::protocol::encode_adelie(
      header, payload, payload_length, response, sizeof(response));
  if (length != 0U) {
    (void)korora_ble::send_to_adelie(fairy::config::korora_address,
                                     fairy::transport::Channel::adelie,
                                     response, length, transfer_id,
                                     fairy::transport::response, receive_ticks);
  }
}

std::size_t make_session_command(fairy::protocol::Opcode opcode,
                                 std::uint32_t session,
                                 std::uint8_t *destination,
                                 std::size_t capacity) {
  fairy::protocol::MessageHeader header;
  header.opcode = opcode;
  header.flags =
      fairy::protocol::require_response | fairy::protocol::execute_immediately;
  header.command_id =
      static_cast<std::uint32_t>(korora_time::now() & 0xFFFF'FFFFULL);
  header.session_id = session;
  header.deadline_ms = 2000;
  return fairy::protocol::encode_adelie(header, nullptr, 0, destination,
                                        capacity);
}

void propagate_session(fairy::protocol::Opcode opcode, std::uint32_t session) {
  std::uint8_t message[fairy::protocol::adelie_max_message_size]{};
  const std::size_t length =
      make_session_command(opcode, session, message, sizeof(message));
  if (length == 0U) {
    return;
  }
  korora_fairies::queue_command_for_all(message, length);
  (void)korora_ble::send_to_galapagos(fairy::transport::Channel::adelie,
                                      message, length);
}

bool every_fairy_assigned() {
  const std::size_t total = korora_fairies::count();
  if (total == 0U) {
    return false;
  }
  for (std::size_t index = 0; index < total; ++index) {
    korora_fairies::NodeSnapshot node;
    if (!korora_fairies::snapshot(index, node) || node.logical_slot == 0xFFU) {
      return false;
    }
  }
  return true;
}

fairy::protocol::Status begin_session(std::uint32_t wanted,
                                      bool require_synchronization) {
  if (wanted == 0U || !every_fairy_assigned()) {
    return wanted == 0U ? fairy::protocol::Status::invalid_parameter
                        : fairy::protocol::Status::inventory_mismatch;
  }
  if (!korora_ble::galapagos_connected()) {
    return fairy::protocol::Status::transport_error;
  }
  if (require_synchronization) {
    for (std::size_t index = 0; index < korora_fairies::count(); ++index) {
      korora_fairies::NodeSnapshot node;
      if (!korora_fairies::snapshot(index, node) || !node.synchronized) {
        return fairy::protocol::Status::not_synchronized;
      }
    }
    if (!korora_galapagos::quality().valid) {
      return fairy::protocol::Status::not_synchronized;
    }
  }
  const std::uint32_t current =
      static_cast<std::uint32_t>(atomic_get(&session_id));
  if (current != 0U && current != wanted) {
    return fairy::protocol::Status::busy;
  }
  atomic_set(&session_id, static_cast<atomic_val_t>(wanted));
  korora_experiment::set_session(wanted);
  korora_local_sensors::set_session(wanted);
  propagate_session(fairy::protocol::Opcode::start_session, wanted);
  return fairy::protocol::Status::accepted;
}

void end_session(std::uint32_t ended) {
  korora_experiment::stop_sync_test();
  korora_experiment::stop_ttl_train();
  propagate_session(fairy::protocol::Opcode::stop_session, ended);
  korora_experiment::set_session(0);
  korora_local_sensors::set_session(0);
  atomic_clear(&session_id);
  diagnostic_session_active = false;
}

void handle_local(const fairy::protocol::AdelieMessageView &command,
                  std::uint16_t transfer_id, std::uint64_t receive_ticks) {
  fairy::protocol::Status status = fairy::protocol::Status::ok;
  std::uint8_t response_payload[fairy::protocol::adelie_max_payload]{};
  fairy::protocol::TlvWriter response_fields(response_payload,
                                             sizeof(response_payload));

  switch (command.header.opcode) {
  case fairy::protocol::Opcode::ping:
    break;

  case fairy::protocol::Opcode::get_inventory: {
    std::uint8_t inventory[fairy::config::max_fairies * 18U]{};
    const std::size_t length =
        korora_fairies::inventory_bytes(inventory, sizeof(inventory));
    if (length != 0U) {
      (void)response_fields.bytes(
          static_cast<std::uint16_t>(fairy::protocol::CommandField::uuid_list),
          inventory, static_cast<std::uint8_t>(length));
    }
    break;
  }

  case fairy::protocol::Opcode::apply_inventory: {
    const std::uint8_t *packed{};
    std::size_t length{};
    if (!command_bytes(command, fairy::protocol::CommandField::uuid_list,
                       packed, length)) {
      status = fairy::protocol::Status::invalid_parameter;
    } else {
      status = korora_fairies::apply_inventory(packed, length);
    }
    break;
  }

  case fairy::protocol::Opcode::get_health: {
    (void)response_fields.u32(
        static_cast<std::uint16_t>(fairy::protocol::Field::transport_errors),
        korora_rs485::errors());
    (void)response_fields.u32(
        static_cast<std::uint16_t>(fairy::protocol::Field::retry_count),
        korora_rs485::retries());
    (void)response_fields.i32(
        static_cast<std::uint16_t>(fairy::protocol::Field::rssi_dbm),
        korora_ble::galapagos_rssi());
    (void)response_fields.u8(
        static_cast<std::uint16_t>(fairy::protocol::Field::state),
        korora_ble::galapagos_connected() ? 1U : 0U);
    const auto sensors = korora_local_sensors::snapshot();
    (void)response_fields.u8(
        static_cast<std::uint16_t>(fairy::protocol::Field::sensor_status),
        sensors.status);
    (void)response_fields.u32(
        static_cast<std::uint16_t>(fairy::protocol::Field::sample_count),
        sensors.sample_count);
    (void)response_fields.u32(
        static_cast<std::uint16_t>(fairy::protocol::Field::sensor_i2c_errors),
        sensors.i2c_errors);
    break;
  }

  case fairy::protocol::Opcode::set_telemetry: {
    std::uint8_t raw{};
    if (!command_u8(command, fairy::protocol::CommandField::telemetry_level,
                    raw) ||
        raw >
            static_cast<std::uint8_t>(fairy::protocol::TelemetryLevel::full)) {
      status = fairy::protocol::Status::invalid_parameter;
    } else {
      const auto level = static_cast<fairy::protocol::TelemetryLevel>(raw);
      korora_fairies::set_telemetry(level);
      korora_local_sensors::set_telemetry(level);
    }
    break;
  }

  case fairy::protocol::Opcode::set_status_led: {
    std::uint8_t led_index{};
    std::uint8_t enabled{};
    if (!command_u8(command, fairy::protocol::CommandField::led_index,
                    led_index) ||
        !command_u8(command, fairy::protocol::CommandField::enabled, enabled) ||
        led_index < 1U || led_index > 2U || enabled > 1U) {
      status = fairy::protocol::Status::invalid_parameter;
    } else {
      // Protocol/UI uses schematic labels D1/D2. Hardware channels are
      // D1 -> nPM LED2 and D2 -> nPM LED1. D3/nPM LED0 is reserved READY.
      const std::uint8_t pmic_led_index = 3U - led_index;
      if (!korora_local_sensors::set_status_led(pmic_led_index,
                                                 enabled != 0U)) {
        status = fairy::protocol::Status::invalid_state;
      }
    }
    break;
  }

  case fairy::protocol::Opcode::clock_exchange:
    (void)response_fields.u64(
        static_cast<std::uint16_t>(
            fairy::protocol::CommandField::clock_t2_ticks),
        receive_ticks);
    (void)response_fields.u64(
        static_cast<std::uint16_t>(
            fairy::protocol::CommandField::clock_t3_ticks),
        korora_time::now());
    break;

  case fairy::protocol::Opcode::start_session:
    status = begin_session(command.header.session_id, true);
    break;

  case fairy::protocol::Opcode::stop_session: {
    const std::uint32_t current =
        static_cast<std::uint32_t>(atomic_get(&session_id));
    if (current != 0U && command.header.session_id != current) {
      status = fairy::protocol::Status::session_mismatch;
    } else if (current != 0U) {
      end_session(current);
    }
    break;
  }

  case fairy::protocol::Opcode::start_ttl_train: {
    std::uint32_t frequency{};
    std::uint32_t width{};
    std::uint32_t count{};
    if (!command_u32(command,
                     fairy::protocol::CommandField::ttl_frequency_millihz,
                     frequency) ||
        !command_u32(command, fairy::protocol::CommandField::ttl_width_us,
                     width)) {
      status = fairy::protocol::Status::invalid_parameter;
    } else {
      (void)command_u32(command, fairy::protocol::CommandField::ttl_count,
                        count);
      bool opened_diagnostic_session = false;
      if (atomic_get(&session_id) == 0) {
        status = begin_session(diagnostic_session, false);
        opened_diagnostic_session = status == fairy::protocol::Status::accepted;
        if (opened_diagnostic_session) {
          diagnostic_session_active = true;
        }
      }
      if (status == fairy::protocol::Status::ok ||
          status == fairy::protocol::Status::accepted) {
        if (korora_experiment::start_ttl_train(frequency, width, count)) {
          status = fairy::protocol::Status::accepted;
        } else {
          status = fairy::protocol::Status::invalid_state;
          if (opened_diagnostic_session) {
            end_session(diagnostic_session);
          }
        }
      }
    }
    break;
  }

  case fairy::protocol::Opcode::stop_ttl_train:
    korora_experiment::stop_ttl_train();
    if (diagnostic_session_active && !korora_experiment::sync_test_active()) {
      end_session(diagnostic_session);
    }
    break;

  case fairy::protocol::Opcode::start_sync_test: {
    std::uint32_t interval = 1000;
    std::uint32_t width = 100;
    (void)command_u32(command,
                      fairy::protocol::CommandField::test_command_interval_ms,
                      interval);
    (void)command_u32(command, fairy::protocol::CommandField::ttl_width_us,
                      width);
    if (atomic_get(&session_id) == 0) {
      status = begin_session(diagnostic_session, false);
      diagnostic_session_active = status == fairy::protocol::Status::accepted;
    }
    if ((status == fairy::protocol::Status::ok ||
         status == fairy::protocol::Status::accepted) &&
        !korora_experiment::start_sync_test(interval, width)) {
      status = fairy::protocol::Status::invalid_parameter;
    } else if (status == fairy::protocol::Status::ok) {
      status = fairy::protocol::Status::accepted;
    }
    break;
  }

  case fairy::protocol::Opcode::stop_sync_test:
    korora_experiment::stop_sync_test();
    if (diagnostic_session_active) {
      end_session(diagnostic_session);
    }
    break;

  default:
    status = fairy::protocol::Status::unsupported;
    break;
  }

  send_response(command, status, transfer_id, response_payload,
                response_fields.size(), receive_ticks);
}

} // namespace

void initialize() {
  k_mutex_init(&control_mutex);
  atomic_clear(&session_id);
  diagnostic_session_active = false;
}

void receive_from_adelie(const fairy::transport::MessageView &message,
                         std::uint64_t receive_ticks) {
  fairy::protocol::AdelieMessageView command;
  if (!fairy::protocol::decode_adelie(message.payload, message.payload_length,
                                      command) ||
      command.header.kind != fairy::protocol::MessageKind::command) {
    return;
  }

  k_mutex_lock(&control_mutex, K_FOREVER);
  if (message.destination == fairy::config::korora_address) {
    handle_local(command, message.transfer_id, receive_ticks);
  } else if (message.destination == fairy::config::galapagos_address) {
    if (!korora_ble::send_to_galapagos(fairy::transport::Channel::adelie,
                                       message.payload, message.payload_length,
                                       message.transfer_id, message.flags)) {
      send_response(command, fairy::protocol::Status::transport_error,
                    message.transfer_id, nullptr, 0, receive_ticks);
    }
  } else if (fairy::config::is_fairy_address(message.destination) ||
             fairy::config::is_discovery_address(message.destination)) {
    if (!korora_fairies::queue_command(message.destination, message.payload,
                                       message.payload_length,
                                       message.transfer_id)) {
      send_response(command, fairy::protocol::Status::queue_full,
                    message.transfer_id, nullptr, 0, receive_ticks);
    }
  } else if (message.destination == fairy::config::broadcast_address) {
    korora_fairies::queue_command_for_all(message.payload,
                                          message.payload_length);
    (void)korora_ble::send_to_galapagos(fairy::transport::Channel::adelie,
                                        message.payload,
                                        message.payload_length);
    send_response(command, fairy::protocol::Status::accepted,
                  message.transfer_id, nullptr, 0, receive_ticks);
  } else {
    send_response(command, fairy::protocol::Status::invalid_parameter,
                  message.transfer_id, nullptr, 0, receive_ticks);
  }
  k_mutex_unlock(&control_mutex);
}

void adelie_disconnected() {
  k_mutex_lock(&control_mutex, K_FOREVER);
  const std::uint32_t current =
      static_cast<std::uint32_t>(atomic_get(&session_id));
  if (current != 0U) {
    end_session(current);
  }
  k_mutex_unlock(&control_mutex);
}

void dependency_disconnected(const char *reason) {
  k_mutex_lock(&control_mutex, K_FOREVER);
  const std::uint32_t current =
      static_cast<std::uint32_t>(atomic_get(&session_id));
  if (current != 0U) {
    korora_experiment::publish_fault(reason);
    end_session(current);
  }
  k_mutex_unlock(&control_mutex);
}

std::uint32_t active_session() {
  return static_cast<std::uint32_t>(atomic_get(&session_id));
}

} // namespace korora_control
