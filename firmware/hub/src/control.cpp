#include "control.hpp"

#include <array>
#include <cstring>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "ble_gateway.hpp"
#include "experiment.hpp"
#include "node_manager.hpp"
#include "rig_protocol/command_protocol.hpp"
#include "rig_protocol/record_protocol.hpp"
#include "rig_protocol/system_config.hpp"
#include "rig_protocol/tlv.hpp"
#include "ttl_generator_manager.hpp"
#include "local_sensors.hpp"
#include "rs485_bus.hpp"
#include "timebase.hpp"

namespace hub_control {
namespace {

inline constexpr std::uint32_t diagnostic_session = 0x53594E43U;

atomic_t session_id;
bool diagnostic_session_active;
k_mutex control_mutex;

bool command_u8(const rig::protocol::CommandMessageView &message,
                rig::protocol::CommandField wanted, std::uint8_t &value) {
  rig::protocol::TlvReader reader(message.payload,
                                    message.header.payload_length);
  rig::protocol::FieldView field;
  while (reader.next(field)) {
    if (field.tag == static_cast<std::uint16_t>(wanted)) {
      return rig::protocol::TlvReader::as_u8(field, value);
    }
  }
  return false;
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

void send_response(const rig::protocol::CommandMessageView &command,
                   rig::protocol::Status status, std::uint16_t transfer_id,
                   const std::uint8_t *payload = nullptr,
                   std::size_t payload_length = 0,
                   std::uint64_t receive_ticks = 0) {
  rig::protocol::MessageHeader header = command.header;
  header.kind = rig::protocol::MessageKind::response;
  header.status = status;
  header.flags = 0;
  header.execute_at_ticks = 0;
  header.payload_length = 0;
  std::uint8_t response[rig::protocol::command_max_message_size]{};
  const std::size_t length = rig::protocol::encode_command(
      header, payload, payload_length, response, sizeof(response));
  if (length != 0U) {
    (void)hub_ble::send_to_host(rig::config::hub_address,
                                     rig::transport::Channel::command,
                                     response, length, transfer_id,
                                     rig::transport::response, receive_ticks);
  }
}

std::size_t make_session_command(rig::protocol::Opcode opcode,
                                 std::uint32_t session,
                                 std::uint8_t *destination,
                                 std::size_t capacity) {
  rig::protocol::MessageHeader header;
  header.opcode = opcode;
  header.flags =
      rig::protocol::require_response | rig::protocol::execute_immediately;
  header.command_id =
      static_cast<std::uint32_t>(hub_time::now() & 0xFFFF'FFFFULL);
  header.session_id = session;
  header.deadline_ms = 2000;
  return rig::protocol::encode_command(header, nullptr, 0, destination,
                                        capacity);
}

void propagate_session(rig::protocol::Opcode opcode, std::uint32_t session) {
  std::uint8_t message[rig::protocol::command_max_message_size]{};
  const std::size_t length =
      make_session_command(opcode, session, message, sizeof(message));
  if (length == 0U) {
    return;
  }
  hub_nodes::queue_command_for_all(message, length);
  (void)hub_ble::send_to_ttl_generator(rig::transport::Channel::command,
                                      message, length);
}

/* Node numbers are flashed into the boards, so every present node already
 * has its slot. Which nodes a session needs is Adelie's decision; the hub
 * only refuses a session with no nodes at all. */
bool any_node_present() { return hub_nodes::count() != 0U; }

rig::protocol::Status begin_session(std::uint32_t wanted,
                                      bool require_synchronization) {
  if (wanted == 0U || !any_node_present()) {
    return wanted == 0U ? rig::protocol::Status::invalid_parameter
                        : rig::protocol::Status::inventory_mismatch;
  }
  if (!hub_ble::ttl_generator_connected()) {
    return rig::protocol::Status::transport_error;
  }
  if (require_synchronization) {
    for (std::size_t index = 0; index < hub_nodes::count(); ++index) {
      hub_nodes::NodeSnapshot node;
      if (!hub_nodes::snapshot(index, node) || !node.synchronized) {
        return rig::protocol::Status::not_synchronized;
      }
    }
    if (!hub_ttl_generator::quality().valid) {
      return rig::protocol::Status::not_synchronized;
    }
  }
  const std::uint32_t current =
      static_cast<std::uint32_t>(atomic_get(&session_id));
  if (current != 0U && current != wanted) {
    return rig::protocol::Status::busy;
  }
  atomic_set(&session_id, static_cast<atomic_val_t>(wanted));
  hub_experiment::set_session(wanted);
  hub_local_sensors::set_session(wanted);
  propagate_session(rig::protocol::Opcode::start_session, wanted);
  return rig::protocol::Status::accepted;
}

void end_session(std::uint32_t ended) {
  hub_experiment::stop_sync_test();
  hub_experiment::stop_ttl_train();
  propagate_session(rig::protocol::Opcode::stop_session, ended);
  hub_experiment::set_session(0);
  hub_local_sensors::set_session(0);
  atomic_clear(&session_id);
  diagnostic_session_active = false;
}

void handle_local(const rig::protocol::CommandMessageView &command,
                  std::uint16_t transfer_id, std::uint64_t receive_ticks) {
  rig::protocol::Status status = rig::protocol::Status::ok;
  std::uint8_t response_payload[rig::protocol::command_max_payload]{};
  rig::protocol::TlvWriter response_fields(response_payload,
                                             sizeof(response_payload));

  switch (command.header.opcode) {
  case rig::protocol::Opcode::ping:
    break;

  case rig::protocol::Opcode::get_inventory: {
    std::uint8_t inventory[rig::config::max_nodes * 18U]{};
    const std::size_t length =
        hub_nodes::inventory_bytes(inventory, sizeof(inventory));
    if (length != 0U) {
      (void)response_fields.bytes(
          static_cast<std::uint16_t>(rig::protocol::CommandField::uuid_list),
          inventory, static_cast<std::uint8_t>(length));
    }
    break;
  }

  case rig::protocol::Opcode::get_health: {
    (void)response_fields.u32(
        static_cast<std::uint16_t>(rig::protocol::Field::transport_errors),
        hub_rs485::errors());
    (void)response_fields.u32(
        static_cast<std::uint16_t>(rig::protocol::Field::retry_count),
        hub_rs485::retries());
    (void)response_fields.i32(
        static_cast<std::uint16_t>(rig::protocol::Field::rssi_dbm),
        hub_ble::ttl_generator_rssi());
    (void)response_fields.u8(
        static_cast<std::uint16_t>(rig::protocol::Field::state),
        hub_ble::ttl_generator_connected() ? 1U : 0U);
    const auto sensors = hub_local_sensors::snapshot();
    (void)response_fields.u8(
        static_cast<std::uint16_t>(rig::protocol::Field::sensor_status),
        sensors.status);
    (void)response_fields.u32(
        static_cast<std::uint16_t>(rig::protocol::Field::sample_count),
        sensors.sample_count);
    (void)response_fields.u32(
        static_cast<std::uint16_t>(rig::protocol::Field::sensor_i2c_errors),
        sensors.i2c_errors);
    break;
  }

  case rig::protocol::Opcode::set_telemetry: {
    std::uint8_t raw{};
    if (!command_u8(command, rig::protocol::CommandField::telemetry_level,
                    raw) ||
        raw >
            static_cast<std::uint8_t>(rig::protocol::TelemetryLevel::full)) {
      status = rig::protocol::Status::invalid_parameter;
    } else {
      const auto level = static_cast<rig::protocol::TelemetryLevel>(raw);
      hub_nodes::set_telemetry(level);
      hub_local_sensors::set_telemetry(level);
    }
    break;
  }

  case rig::protocol::Opcode::set_status_led: {
    std::uint8_t led_index{};
    std::uint8_t enabled{};
    if (!command_u8(command, rig::protocol::CommandField::led_index,
                    led_index) ||
        !command_u8(command, rig::protocol::CommandField::enabled, enabled) ||
        led_index < 1U || led_index > 2U || enabled > 1U) {
      status = rig::protocol::Status::invalid_parameter;
    } else {
      // Protocol/UI uses schematic labels D1/D2. Hardware channels are
      // D1 -> nPM LED2 and D2 -> nPM LED1. D3/nPM LED0 is reserved READY.
      const std::uint8_t pmic_led_index = 3U - led_index;
      if (!hub_local_sensors::set_status_led(pmic_led_index,
                                                 enabled != 0U)) {
        status = rig::protocol::Status::invalid_state;
      }
    }
    break;
  }

  case rig::protocol::Opcode::clock_exchange:
    (void)response_fields.u64(
        static_cast<std::uint16_t>(
            rig::protocol::CommandField::clock_t2_ticks),
        receive_ticks);
    (void)response_fields.u64(
        static_cast<std::uint16_t>(
            rig::protocol::CommandField::clock_t3_ticks),
        hub_time::now());
    break;

  case rig::protocol::Opcode::start_session:
    status = begin_session(command.header.session_id, true);
    break;

  case rig::protocol::Opcode::stop_session: {
    const std::uint32_t current =
        static_cast<std::uint32_t>(atomic_get(&session_id));
    if (current != 0U && command.header.session_id != current) {
      status = rig::protocol::Status::session_mismatch;
    } else if (current != 0U) {
      end_session(current);
    }
    break;
  }

  case rig::protocol::Opcode::start_ttl_train: {
    std::uint32_t frequency{};
    std::uint32_t width{};
    std::uint32_t count{};
    if (!command_u32(command,
                     rig::protocol::CommandField::ttl_frequency_millihz,
                     frequency) ||
        !command_u32(command, rig::protocol::CommandField::ttl_width_us,
                     width)) {
      status = rig::protocol::Status::invalid_parameter;
    } else {
      (void)command_u32(command, rig::protocol::CommandField::ttl_count,
                        count);
      bool opened_diagnostic_session = false;
      if (atomic_get(&session_id) == 0) {
        status = begin_session(diagnostic_session, false);
        opened_diagnostic_session = status == rig::protocol::Status::accepted;
        if (opened_diagnostic_session) {
          diagnostic_session_active = true;
        }
      }
      if (status == rig::protocol::Status::ok ||
          status == rig::protocol::Status::accepted) {
        if (hub_experiment::start_ttl_train(frequency, width, count)) {
          status = rig::protocol::Status::accepted;
        } else {
          status = rig::protocol::Status::invalid_state;
          if (opened_diagnostic_session) {
            end_session(diagnostic_session);
          }
        }
      }
    }
    break;
  }

  case rig::protocol::Opcode::stop_ttl_train:
    hub_experiment::stop_ttl_train();
    if (diagnostic_session_active && !hub_experiment::sync_test_active()) {
      end_session(diagnostic_session);
    }
    break;

  case rig::protocol::Opcode::start_sync_test: {
    std::uint32_t interval = 5000;
    std::uint32_t width = 100;
    (void)command_u32(command,
                      rig::protocol::CommandField::test_command_interval_ms,
                      interval);
    (void)command_u32(command, rig::protocol::CommandField::ttl_width_us,
                      width);
    if (atomic_get(&session_id) == 0) {
      status = begin_session(diagnostic_session, false);
      diagnostic_session_active = status == rig::protocol::Status::accepted;
    }
    if ((status == rig::protocol::Status::ok ||
         status == rig::protocol::Status::accepted) &&
        !hub_experiment::start_sync_test(interval, width)) {
      status = rig::protocol::Status::invalid_parameter;
    } else if (status == rig::protocol::Status::ok) {
      status = rig::protocol::Status::accepted;
    }
    break;
  }

  case rig::protocol::Opcode::stop_sync_test:
    hub_experiment::stop_sync_test();
    if (diagnostic_session_active) {
      end_session(diagnostic_session);
    }
    break;

  default:
    status = rig::protocol::Status::unsupported;
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

void receive_from_host(const rig::transport::MessageView &message,
                         std::uint64_t receive_ticks) {
  rig::protocol::CommandMessageView command;
  if (!rig::protocol::decode_command(message.payload, message.payload_length,
                                      command) ||
      command.header.kind != rig::protocol::MessageKind::command) {
    return;
  }

  k_mutex_lock(&control_mutex, K_FOREVER);
  if (message.destination == rig::config::hub_address) {
    handle_local(command, message.transfer_id, receive_ticks);
  } else if (message.destination == rig::config::ttl_generator_address) {
    if (!hub_ble::send_to_ttl_generator(rig::transport::Channel::command,
                                       message.payload, message.payload_length,
                                       message.transfer_id, message.flags)) {
      send_response(command, rig::protocol::Status::transport_error,
                    message.transfer_id, nullptr, 0, receive_ticks);
    }
  } else if (rig::config::is_node_address(message.destination)) {
    if (!hub_nodes::queue_command(message.destination, message.payload,
                                       message.payload_length,
                                       message.transfer_id)) {
      send_response(command, rig::protocol::Status::queue_full,
                    message.transfer_id, nullptr, 0, receive_ticks);
    }
  } else if (message.destination == rig::config::broadcast_address) {
    hub_nodes::queue_command_for_all(message.payload,
                                          message.payload_length);
    (void)hub_ble::send_to_ttl_generator(rig::transport::Channel::command,
                                        message.payload,
                                        message.payload_length);
    send_response(command, rig::protocol::Status::accepted,
                  message.transfer_id, nullptr, 0, receive_ticks);
  } else {
    send_response(command, rig::protocol::Status::invalid_parameter,
                  message.transfer_id, nullptr, 0, receive_ticks);
  }
  k_mutex_unlock(&control_mutex);
}

void host_disconnected() {
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
    hub_experiment::publish_fault(reason);
    end_session(current);
  }
  k_mutex_unlock(&control_mutex);
}

std::uint32_t active_session() {
  return static_cast<std::uint32_t>(atomic_get(&session_id));
}

} // namespace hub_control
