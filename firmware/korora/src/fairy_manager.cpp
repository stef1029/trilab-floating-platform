#include "fairy_manager.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>

#include "ble_gateway.hpp"
#include "control.hpp"
#include "debug_log.hpp"
#include "fairy_shared/bytes.hpp"
#include "fairy_shared/fairy_protocol.hpp"
#include "fairy_shared/system_config.hpp"
#include "fairy_shared/tlv.hpp"
#include "rs485_bus.hpp"
#include "timebase.hpp"

namespace korora_fairies {
namespace {

struct Node {
  fairy::protocol::DeviceUuid uuid{};
  std::uint8_t address{};
  std::uint8_t logical_slot{0xFF};
  std::uint32_t capabilities{};
  bool present{};
  bool queue_drained{};
  bool have_pair{};
  std::uint32_t last_pulse{};
  std::uint64_t last_local{};
  std::uint32_t last_seen_ms{};
  std::uint32_t next_poll_ms{};
  std::uint32_t transport_errors{};
  std::uint64_t last_quality_ticks{};
  fairy::time::AffineClockModel<16> model{};
};

struct Command {
  std::uint8_t destination{};
  std::uint16_t length{};
  std::uint16_t host_transfer{};
  std::array<std::uint8_t, fairy::protocol::adelie_max_message_size> bytes{};
};

std::array<Node, fairy::config::max_fairies> nodes;
std::size_t node_count;
std::uint32_t inventory_generation;
K_MUTEX_DEFINE(nodes_mutex);
K_MSGQ_DEFINE(commands, sizeof(Command), 24, 4);
K_SEM_DEFINE(start_sem, 0, 1);
fairy::protocol::TelemetryLevel current_telemetry =
    fairy::protocol::TelemetryLevel::standard;
std::uint32_t nonce_state = 0x4B4F524FU;
inline constexpr std::uint32_t assigned_poll_interval_ms = 10;

std::uint32_t next_nonce() {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  nonce_state ^= nonce_state << 13U;
  nonce_state ^= nonce_state >> 17U;
  nonce_state ^= nonce_state << 5U;
  const std::uint32_t result = nonce_state;
  k_mutex_unlock(&nodes_mutex);
  return result;
}

Node *find_address(std::uint8_t address) {
  for (std::size_t i = 0; i < node_count; ++i) {
    if (nodes[i].present && nodes[i].address == address) {
      return &nodes[i];
    }
  }
  return nullptr;
}

Node *find_uuid(const fairy::protocol::DeviceUuid &uuid) {
  for (std::size_t i = 0; i < node_count; ++i) {
    if (nodes[i].uuid == uuid) {
      return &nodes[i];
    }
  }
  return nullptr;
}

fairy::protocol::Status assign(Node &node, std::uint8_t address,
                               std::uint8_t logical_slot) {
  fairy::protocol::Assignment assignment;
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  assignment.uuid = node.uuid;
  k_mutex_unlock(&nodes_mutex);
  assignment.address = address;
  assignment.logical_slot = logical_slot;
  std::uint8_t assignment_payload[16]{};
  const std::size_t assignment_length = fairy::protocol::encode_assignment(
      assignment, assignment_payload, sizeof(assignment_payload));
  fairy::protocol::MagellanHeader header;
  header.type = fairy::protocol::MagellanType::assign;
  header.nonce = next_nonce();
  std::uint8_t message[48]{};
  const std::size_t message_length = fairy::protocol::encode_magellan(
      header, assignment_payload, assignment_length, message, sizeof(message));
  korora_rs485::Received response;
  const int error = korora_rs485::exchange(
      fairy::config::broadcast_address, fairy::transport::Channel::magellan,
      message, message_length, response, 6);
  if (error != 0) {
    k_mutex_lock(&nodes_mutex, K_FOREVER);
    ++node.transport_errors;
    k_mutex_unlock(&nodes_mutex);
    return fairy::protocol::Status::transport_error;
  }
  fairy::protocol::MagellanView response_view;
  fairy::protocol::Assignment response_assignment;
  if (!fairy::protocol::decode_magellan(response.payload.data(),
                                        response.length, response_view) ||
      response_view.header.type != fairy::protocol::MagellanType::assigned ||
      !fairy::protocol::decode_assignment(response_view.payload,
                                          response_view.header.payload_length,
                                          response_assignment) ||
      response_assignment.uuid != assignment.uuid ||
      response_assignment.address != address) {
    k_mutex_lock(&nodes_mutex, K_FOREVER);
    ++node.transport_errors;
    k_mutex_unlock(&nodes_mutex);
    return fairy::protocol::Status::bad_message;
  }
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  node.address = address;
  node.logical_slot = logical_slot;
  node.queue_drained = false;
  node.have_pair = false;
  node.present = true;
  node.last_seen_ms = static_cast<std::uint32_t>(k_uptime_get());
  node.model.reset();
  k_mutex_unlock(&nodes_mutex);
  return fairy::protocol::Status::ok;
}

void publish_record(std::uint8_t source, fairy::protocol::RecordType type,
                    std::uint16_t flags, std::uint32_t record_id,
                    std::uint32_t session_id, std::uint64_t ticks,
                    const std::uint8_t *payload, std::size_t payload_length,
                    bool diagnostic_priority = false) {
  fairy::protocol::RecordHeader header;
  header.type = type;
  header.flags = flags;
  header.record_id = record_id;
  header.session_id = session_id;
  header.timestamp_ticks = ticks;
  header.clock_hz = fairy::config::common_timer_hz;
  std::uint8_t record[fairy::protocol::fairy_max_record_size]{};
  const std::size_t length = fairy::protocol::encode_record(
      header, payload, payload_length, record, sizeof(record));
  if (length != 0U) {
    (void)korora_ble::send_to_adelie(source, fairy::transport::Channel::fairy,
                                     record, length, 0, 0, 0,
                                     diagnostic_priority);
  }
}

void publish_quality(Node &node, std::uint64_t now_ticks) {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  if (!node.present ||
      current_telemetry == fairy::protocol::TelemetryLevel::critical) {
    k_mutex_unlock(&nodes_mutex);
    return;
  }
  if (now_ticks - node.last_quality_ticks < 16'000'000ULL) {
    k_mutex_unlock(&nodes_mutex);
    return;
  }
  node.last_quality_ticks = now_ticks;
  const auto quality = node.model.quality();
  const std::uint8_t address = node.address;
  const std::uint32_t errors = node.transport_errors;
  k_mutex_unlock(&nodes_mutex);
  std::uint8_t payload[96]{};
  fairy::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.i64(static_cast<std::uint16_t>(fairy::protocol::Field::rms_ns),
                   static_cast<std::int64_t>(quality.rms_ticks * 62.5));
  (void)fields.i64(static_cast<std::uint16_t>(fairy::protocol::Field::skew_ppb),
                   quality.skew_ppb);
  (void)fields.u8(
      static_cast<std::uint16_t>(fairy::protocol::Field::model_points),
      static_cast<std::uint8_t>(quality.points));
  (void)fields.u32(
      static_cast<std::uint16_t>(fairy::protocol::Field::model_generation),
      quality.generation);
  (void)fields.u32(
      static_cast<std::uint16_t>(fairy::protocol::Field::transport_errors),
      errors);
  const std::uint16_t flags =
      quality.valid ? fairy::protocol::synchronized : 0U;
  publish_record(address, fairy::protocol::RecordType::sync_quality, flags,
                 quality.generation, 0, now_ticks, payload, fields.size());
}

void publish_inventory(const Node &node) {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  const fairy::protocol::DeviceUuid uuid = node.uuid;
  const std::uint8_t address = node.address;
  const std::uint8_t logical_slot = node.logical_slot;
  const std::uint32_t capabilities = node.capabilities;
  const bool present = node.present;
  k_mutex_unlock(&nodes_mutex);
  std::uint8_t payload[64]{};
  fairy::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.bytes(static_cast<std::uint16_t>(fairy::protocol::Field::uuid),
                     uuid.data(), static_cast<std::uint8_t>(uuid.size()));
  (void)fields.u8(
      static_cast<std::uint16_t>(fairy::protocol::Field::link_address),
      address);
  (void)fields.u8(
      static_cast<std::uint16_t>(fairy::protocol::Field::logical_slot),
      logical_slot);
  (void)fields.u32(static_cast<std::uint16_t>(fairy::protocol::Field::value),
                   capabilities);
  (void)fields.boolean(
      static_cast<std::uint16_t>(fairy::protocol::Field::state), present);
  publish_record(fairy::config::korora_address,
                 fairy::protocol::RecordType::inventory,
                 fairy::protocol::critical, 0, 0, korora_time::now(), payload,
                 fields.size(), true);
}

void process_sync(Node &node, const fairy::protocol::RecordView &record) {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  if (!node.queue_drained) {
    k_mutex_unlock(&nodes_mutex);
    return;
  }
  const std::uint32_t current_pulse = korora_time::pulse_count();
  std::uint32_t paired_pulse = current_pulse;
  if (node.have_pair) {
    const std::uint64_t local_delta =
        record.header.timestamp_ticks - node.last_local;
    const std::uint32_t intervals =
        static_cast<std::uint32_t>((local_delta + 2'000'000ULL) / 4'000'000ULL);
    if (intervals == 0U || intervals > 4U) {
      node.model.reset();
      node.have_pair = false;
      k_mutex_unlock(&nodes_mutex);
      return;
    }
    paired_pulse = node.last_pulse + intervals;
    if (paired_pulse > current_pulse || current_pulse - paired_pulse > 1U) {
      node.model.reset();
      node.have_pair = false;
      k_mutex_unlock(&nodes_mutex);
      return;
    }
  }

  const std::uint64_t hub_ticks = korora_time::pulse_ticks(paired_pulse);
  if (!node.model.add(record.header.timestamp_ticks, hub_ticks)) {
    k_mutex_unlock(&nodes_mutex);
    return;
  }
  node.have_pair = true;
  node.last_pulse = paired_pulse;
  node.last_local = record.header.timestamp_ticks;

  const bool publish_pair =
      current_telemetry == fairy::protocol::TelemetryLevel::full;
  const std::uint8_t address = node.address;
  k_mutex_unlock(&nodes_mutex);
  if (publish_pair) {
    std::uint8_t payload[56]{};
    fairy::protocol::TlvWriter fields(payload, sizeof(payload));
    (void)fields.u32(
        static_cast<std::uint16_t>(fairy::protocol::Field::sequence),
        paired_pulse);
    (void)fields.u64(
        static_cast<std::uint16_t>(fairy::protocol::Field::requested_ticks),
        hub_ticks);
    (void)fields.u64(
        static_cast<std::uint16_t>(fairy::protocol::Field::actual_ticks),
        record.header.timestamp_ticks);
    publish_record(address, fairy::protocol::RecordType::clock_pair,
                   fairy::protocol::synchronized, record.header.record_id,
                   record.header.session_id, hub_ticks, payload, fields.size());
  }
}

void forward(Node &node, const fairy::protocol::RecordView &record,
             const std::uint8_t *raw, std::size_t raw_length) {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  const fairy::protocol::TelemetryLevel level = current_telemetry;
  const std::uint8_t address = node.address;
  if (!fairy::protocol::record_visible_at(record.header.type, level)) {
    k_mutex_unlock(&nodes_mutex);
    return;
  }

  const bool event =
      record.header.type == fairy::protocol::RecordType::light_gate ||
      record.header.type == fairy::protocol::RecordType::digital_input ||
      record.header.type == fairy::protocol::RecordType::output_change ||
      record.header.type == fairy::protocol::RecordType::command_result;
  if (!event) {
    k_mutex_unlock(&nodes_mutex);
    (void)korora_ble::send_to_adelie(address, fairy::transport::Channel::fairy,
                                     raw, raw_length, 0, 0, 0,
                                     (record.header.flags &
                                      fairy::protocol::critical) != 0U);
    return;
  }

  std::uint8_t payload[fairy::protocol::fairy_max_payload]{};
  std::size_t payload_length = record.header.payload_length;
  std::memcpy(payload, record.payload, payload_length);
  std::uint64_t converted{};
  std::uint16_t flags = record.header.flags;
  if (node.model.predict(record.header.timestamp_ticks, converted)) {
    fairy::protocol::TlvWriter extra(payload + payload_length,
                                     sizeof(payload) - payload_length);
    (void)extra.u64(
        static_cast<std::uint16_t>(fairy::protocol::Field::reference_ticks),
        converted);
    payload_length += extra.size();
    flags |= fairy::protocol::synchronized;
  }
  k_mutex_unlock(&nodes_mutex);
  publish_record(address, record.header.type, flags, record.header.record_id,
                 record.header.session_id, record.header.timestamp_ticks,
                 payload, payload_length, true);
}

void poll(Node &node) {
  const std::uint32_t now_ms = static_cast<std::uint32_t>(k_uptime_get());
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  if (now_ms < node.next_poll_ms) {
    k_mutex_unlock(&nodes_mutex);
    return;
  }
  const bool was_present = node.present;
  const std::uint8_t address = node.address;
  if (address == fairy::config::unassigned_address) {
    node.next_poll_ms = now_ms + 500U;
    k_mutex_unlock(&nodes_mutex);
    return;
  }
  k_mutex_unlock(&nodes_mutex);
  korora_rs485::Received response;
  const int error = korora_rs485::exchange(
      address, fairy::transport::Channel::fairy, nullptr, 0, response, 12,
      fairy::transport::ack_required, true);
  if (error != 0) {
    k_mutex_lock(&nodes_mutex, K_FOREVER);
    ++node.transport_errors;
    node.next_poll_ms = now_ms + (was_present ? 10U : 500U);
    bool became_absent = false;
    if (node.last_seen_ms != 0U &&
        static_cast<std::uint32_t>(k_uptime_get()) - node.last_seen_ms >
            3000U) {
      node.present = false;
      node.model.reset();
      became_absent = was_present;
      if (became_absent) {
        ++inventory_generation;
      }
    }
    k_mutex_unlock(&nodes_mutex);
    if (became_absent) {
      publish_inventory(node);
      korora_control::dependency_disconnected("Fairy disconnected");
    }
    return;
  }
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  node.present = true;
  node.next_poll_ms = now_ms + assigned_poll_interval_ms;
  node.last_seen_ms = static_cast<std::uint32_t>(k_uptime_get());
  if (!was_present) {
    ++inventory_generation;
  }
  k_mutex_unlock(&nodes_mutex);
  if (!was_present) {
    publish_inventory(node);
  }
  if (response.length == 0U) {
    k_mutex_lock(&nodes_mutex, K_FOREVER);
    node.queue_drained = true;
    k_mutex_unlock(&nodes_mutex);
    return;
  }

  fairy::protocol::RecordView record;
  if (!fairy::protocol::decode_record(response.payload.data(), response.length,
                                      record)) {
    k_mutex_lock(&nodes_mutex, K_FOREVER);
    ++node.transport_errors;
    k_mutex_unlock(&nodes_mutex);
    return;
  }
  if (record.header.type == fairy::protocol::RecordType::sync_observation) {
    process_sync(node, record);
  }
  forward(node, record, response.payload.data(), response.length);
}

void command_error(const Command &command, fairy::protocol::Status status) {
  fairy::protocol::AdelieMessageView request;
  if (!fairy::protocol::decode_adelie(command.bytes.data(), command.length,
                                      request)) {
    return;
  }
  fairy::protocol::MessageHeader header = request.header;
  header.kind = fairy::protocol::MessageKind::response;
  header.status = status;
  header.flags = 0;
  std::uint8_t response[fairy::protocol::adelie_max_message_size]{};
  const std::size_t length = fairy::protocol::encode_adelie(
      header, nullptr, 0, response, sizeof(response));
  (void)korora_ble::send_to_adelie(
      fairy::config::korora_address, fairy::transport::Channel::adelie,
      response, length, command.host_transfer, fairy::transport::response);
}

void handle_command(const Command &command) {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  Node *node = find_address(command.destination);
  if (node == nullptr) {
    k_mutex_unlock(&nodes_mutex);
    command_error(command, fairy::protocol::Status::transport_error);
    return;
  }
  const std::uint8_t address = node->address;
  k_mutex_unlock(&nodes_mutex);

  korora_rs485::Received response;
  const int error =
      korora_rs485::exchange(address, fairy::transport::Channel::adelie,
                             command.bytes.data(), command.length, response, 8);
  if (error != 0 || response.channel != fairy::transport::Channel::adelie) {
    command_error(command, fairy::protocol::Status::transport_error);
    return;
  }
  (void)korora_ble::send_to_adelie(
      address, fairy::transport::Channel::adelie, response.payload.data(),
      response.length, command.host_transfer, fairy::transport::response);
}

void release_all() {
  fairy::protocol::MagellanHeader header;
  header.type = fairy::protocol::MagellanType::release;
  header.nonce = next_nonce();
  std::uint8_t message[16]{};
  const std::size_t length = fairy::protocol::encode_magellan(
      header, nullptr, 0, message, sizeof(message));
  (void)korora_rs485::send_one_way(fairy::config::broadcast_address,
                                   fairy::transport::Channel::magellan, 1, 0,
                                   message, length);
  k_sleep(K_MSEC(20));
}

std::size_t discovery_round(std::uint8_t round) {
  korora_rs485::DiscoveredOffer offers[fairy::config::max_fairies]{};
  const std::size_t found = korora_rs485::discover(next_nonce(), round, offers,
                                                   fairy::config::max_fairies);
  std::size_t added = 0;
  for (std::size_t i = 0; i < found; ++i) {
    k_mutex_lock(&nodes_mutex, K_FOREVER);
    Node *existing = find_uuid(offers[i].offer.uuid);
    if (existing != nullptr) {
      const bool was_present = existing->present;
      const std::uint8_t address =
          existing->logical_slot == 0xFFU
              ? fairy::config::discovery_address(
                    static_cast<std::size_t>(existing - nodes.data()))
              : fairy::config::fairy_address(existing->logical_slot);
      const std::uint8_t logical_slot = existing->logical_slot;
      k_mutex_unlock(&nodes_mutex);
      if (assign(*existing, address, logical_slot) ==
          fairy::protocol::Status::ok) {
        ++added;
        if (!was_present) {
          k_mutex_lock(&nodes_mutex, K_FOREVER);
          ++inventory_generation;
          k_mutex_unlock(&nodes_mutex);
          publish_inventory(*existing);
        }
      }
      continue;
    }
    std::size_t node_index = node_count;
    if (node_count >= nodes.size()) {
      node_index = nodes.size();
      for (std::size_t candidate = 0; candidate < node_count; ++candidate) {
        if (!nodes[candidate].present) {
          node_index = candidate;
          break;
        }
      }
      if (node_index == nodes.size()) {
        k_mutex_unlock(&nodes_mutex);
        continue;
      }
    } else {
      ++node_count;
    }
    Node &node = nodes[node_index];
    node = {};
    node.uuid = offers[i].offer.uuid;
    node.capabilities = offers[i].offer.capabilities;
    node.present = false;
    node.address = fairy::config::unassigned_address;
    const std::uint8_t temporary = fairy::config::discovery_address(node_index);
    k_mutex_unlock(&nodes_mutex);

    if (assign(node, temporary, 0xFF) == fairy::protocol::Status::ok) {
      k_mutex_lock(&nodes_mutex, K_FOREVER);
      ++inventory_generation;
      k_mutex_unlock(&nodes_mutex);
      ++added;
      publish_inventory(node);
      k_mutex_lock(&nodes_mutex, K_FOREVER);
      const std::uint8_t uuid0 = node.uuid[0];
      const std::uint8_t uuid1 = node.uuid[1];
      const std::uint8_t assigned_address = node.address;
      k_mutex_unlock(&nodes_mutex);
      korora_debug::log("MAGELLAN uuid=%02x%02x... address=0x%02x\r\n", uuid0,
                        uuid1, assigned_address);
    }
  }
  return added;
}

void manager_thread(void *, void *, void *) {
  k_sem_take(&start_sem, K_FOREVER);
  release_all();
  std::uint8_t round = 0;
  unsigned int empty_rounds = 0;
  while (empty_rounds < 3U) {
    if (discovery_round(round++) == 0U) {
      ++empty_rounds;
    } else {
      empty_rounds = 0;
    }
  }

  std::int64_t next_discovery = k_uptime_get() + 1000;
  while (true) {
    Command command;
    while (k_msgq_get(&commands, &command, K_NO_WAIT) == 0) {
      handle_command(command);
    }

    k_mutex_lock(&nodes_mutex, K_FOREVER);
    const std::size_t count_snapshot = node_count;
    k_mutex_unlock(&nodes_mutex);
    for (std::size_t i = 0; i < count_snapshot; ++i) {
      k_mutex_lock(&nodes_mutex, K_FOREVER);
      Node *node = &nodes[i];
      k_mutex_unlock(&nodes_mutex);
      poll(*node);
      publish_quality(*node, korora_time::now());
    }

    if (k_uptime_get() >= next_discovery) {
      (void)discovery_round(round++);
      next_discovery = k_uptime_get() + 1000;
    }
    k_sleep(K_MSEC(1));
  }
}

K_THREAD_DEFINE(manager_thread_id, 4096, manager_thread, nullptr, nullptr,
                nullptr, 7, 0, 0);

} // namespace

int initialize() {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  node_count = 0;
  inventory_generation = 0;
  current_telemetry = fairy::protocol::TelemetryLevel::standard;
  k_mutex_unlock(&nodes_mutex);
  k_sem_give(&start_sem);
  return 0;
}

std::size_t count() {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  std::size_t result = 0;
  for (std::size_t index = 0; index < node_count; ++index) {
    result += nodes[index].present ? 1U : 0U;
  }
  k_mutex_unlock(&nodes_mutex);
  return result;
}

std::size_t inventory_bytes(std::uint8_t *destination, std::size_t capacity) {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  std::size_t present_count = 0;
  for (std::size_t index = 0; index < node_count; ++index) {
    present_count += nodes[index].present ? 1U : 0U;
  }
  const std::size_t needed = present_count * 18U;
  if (destination == nullptr || capacity < needed) {
    k_mutex_unlock(&nodes_mutex);
    return 0;
  }
  std::size_t offset = 0;
  for (std::size_t i = 0; i < node_count; ++i) {
    if (!nodes[i].present) {
      continue;
    }
    std::memcpy(destination + offset, nodes[i].uuid.data(),
                nodes[i].uuid.size());
    destination[offset + 12] = nodes[i].address;
    destination[offset + 13] = nodes[i].logical_slot;
    fairy::wire::put_u32(destination + offset + 14, nodes[i].capabilities);
    offset += 18;
  }
  k_mutex_unlock(&nodes_mutex);
  return needed;
}

fairy::protocol::Status apply_inventory(const std::uint8_t *packed,
                                        std::size_t length) {
  if (packed == nullptr || length == 0U || length % 13U != 0U) {
    return fairy::protocol::Status::inventory_mismatch;
  }

  k_mutex_lock(&nodes_mutex, K_FOREVER);
  std::size_t present_count = 0;
  for (std::size_t index = 0; index < node_count; ++index) {
    present_count += nodes[index].present ? 1U : 0U;
  }
  if (length / 13U != present_count) {
    k_mutex_unlock(&nodes_mutex);
    return fairy::protocol::Status::inventory_mismatch;
  }
  const std::uint32_t expected_generation = inventory_generation;
  std::array<bool, fairy::config::max_fairies> matched{};
  std::array<bool, fairy::config::max_fairies> slots{};
  for (std::size_t offset = 0; offset < length; offset += 13U) {
    fairy::protocol::DeviceUuid uuid;
    std::memcpy(uuid.data(), packed + offset, uuid.size());
    const std::uint8_t fairy_number = packed[offset + 12];
    if (fairy_number >= fairy::config::max_fairies) {
      k_mutex_unlock(&nodes_mutex);
      return fairy::protocol::Status::invalid_parameter;
    }
    if (slots[fairy_number]) {
      k_mutex_unlock(&nodes_mutex);
      return fairy::protocol::Status::inventory_mismatch;
    }
    slots[fairy_number] = true;
    Node *node = find_uuid(uuid);
    if (node == nullptr) {
      k_mutex_unlock(&nodes_mutex);
      return fairy::protocol::Status::inventory_mismatch;
    }
    if (!node->present) {
      k_mutex_unlock(&nodes_mutex);
      return fairy::protocol::Status::inventory_mismatch;
    }
    const std::size_t index = static_cast<std::size_t>(node - nodes.data());
    if (matched[index]) {
      k_mutex_unlock(&nodes_mutex);
      return fairy::protocol::Status::inventory_mismatch;
    }
    matched[index] = true;
  }
  for (std::size_t index = 0; index < node_count; ++index) {
    if (nodes[index].present && !matched[index]) {
      k_mutex_unlock(&nodes_mutex);
      return fairy::protocol::Status::inventory_mismatch;
    }
  }
  k_mutex_unlock(&nodes_mutex);

  for (std::size_t offset = 0; offset < length; offset += 13U) {
    fairy::protocol::DeviceUuid uuid;
    std::memcpy(uuid.data(), packed + offset, uuid.size());
    const std::uint8_t fairy_number = packed[offset + 12];
    k_mutex_lock(&nodes_mutex, K_FOREVER);
    Node *node = find_uuid(uuid);
    k_mutex_unlock(&nodes_mutex);
    if (node == nullptr ||
        assign(*node, fairy::config::fairy_address(fairy_number),
               fairy_number) != fairy::protocol::Status::ok) {
      return fairy::protocol::Status::transport_error;
    }
  }
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  const bool unchanged = inventory_generation == expected_generation;
  k_mutex_unlock(&nodes_mutex);
  if (!unchanged) {
    return fairy::protocol::Status::inventory_mismatch;
  }
  return fairy::protocol::Status::ok;
}

bool queue_command(std::uint8_t destination, const std::uint8_t *message,
                   std::size_t length, std::uint16_t host_transfer_id) {
  if (message == nullptr || length > fairy::protocol::adelie_max_message_size) {
    return false;
  }
  Command command;
  command.destination = destination;
  command.length = static_cast<std::uint16_t>(length);
  command.host_transfer = host_transfer_id;
  std::memcpy(command.bytes.data(), message, length);
  return k_msgq_put(&commands, &command, K_NO_WAIT) == 0;
}

void queue_command_for_all(const std::uint8_t *message, std::size_t length) {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  for (std::size_t i = 0; i < node_count; ++i) {
    if (nodes[i].present && nodes[i].logical_slot != 0xFFU) {
      (void)queue_command(nodes[i].address, message, length, 0);
    }
  }
  k_mutex_unlock(&nodes_mutex);
}

bool local_to_korora(std::uint8_t address, std::uint64_t local_ticks,
                     std::uint64_t &korora_ticks) {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  Node *node = find_address(address);
  const bool result =
      node != nullptr && node->model.predict(local_ticks, korora_ticks);
  k_mutex_unlock(&nodes_mutex);
  return result;
}

void set_telemetry(fairy::protocol::TelemetryLevel level) {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  current_telemetry = level;
  k_mutex_unlock(&nodes_mutex);
}

fairy::protocol::TelemetryLevel telemetry() {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  const auto result = current_telemetry;
  k_mutex_unlock(&nodes_mutex);
  return result;
}

bool snapshot(std::size_t index, NodeSnapshot &output) {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  const Node *selected = nullptr;
  std::size_t visible_index = 0;
  for (std::size_t raw_index = 0; raw_index < node_count; ++raw_index) {
    if (!nodes[raw_index].present) {
      continue;
    }
    if (visible_index++ == index) {
      selected = &nodes[raw_index];
      break;
    }
  }
  if (selected == nullptr) {
    k_mutex_unlock(&nodes_mutex);
    return false;
  }
  const Node &node = *selected;
  output.uuid = node.uuid;
  output.address = node.address;
  output.logical_slot = node.logical_slot;
  output.capabilities = node.capabilities;
  output.present = node.present;
  output.clock = node.model.quality();
  output.synchronized = output.clock.valid;
  output.transport_errors = node.transport_errors;
  output.last_seen_ms = node.last_seen_ms;
  k_mutex_unlock(&nodes_mutex);
  return true;
}

} // namespace korora_fairies
