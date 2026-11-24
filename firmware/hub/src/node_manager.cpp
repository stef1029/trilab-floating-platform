#include "node_manager.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>

#include "ble_gateway.hpp"
#include "control.hpp"
#include "debug_log.hpp"
#include "rig_protocol/bytes.hpp"
#include "rig_protocol/record_protocol.hpp"
#include "rig_protocol/system_config.hpp"
#include "rig_protocol/tlv.hpp"
#include "rs485_bus.hpp"
#include "timebase.hpp"

namespace hub_nodes {
namespace {

/*
 * One entry per node number. Node numbers are flashed into each board
 * (rig_protocol/node_identity.hpp), so entry i is always address
 * node_address(i). There is no discovery: the hub polls every slot, and a
 * slot is present while its board answers. The UUID is information only,
 * learned from the board's boot record; it stays zero for a board that was
 * already running when the hub started, until that board next resets.
 */
struct Node {
  rig::protocol::DeviceUuid uuid{};
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
  rig::time::AffineClockModel<16> model{};
};

struct Command {
  std::uint8_t destination{};
  std::uint16_t length{};
  std::uint16_t host_transfer{};
  std::array<std::uint8_t, rig::protocol::command_max_message_size> bytes{};
};

std::array<Node, rig::config::max_nodes> nodes;
K_MUTEX_DEFINE(nodes_mutex);
K_MSGQ_DEFINE(commands, sizeof(Command), 24, 4);
K_SEM_DEFINE(start_sem, 0, 1);
rig::protocol::TelemetryLevel current_telemetry =
    rig::protocol::TelemetryLevel::standard;
inline constexpr std::uint32_t assigned_poll_interval_ms = 10;
/* How often an absent slot is asked "are you there?". Each miss costs one
 * 12 ms poll timeout, so with every slot empty this is about 2% of the bus
 * per slot, and a board that powers up late appears within half a second. */
inline constexpr std::uint32_t absent_probe_interval_ms = 500;
inline constexpr std::uint32_t absent_after_ms = 3000;
/* Every node board has the same peripherals; this was the offer's
 * capability field when nodes were discovered. */
inline constexpr std::uint32_t node_capabilities =
    rig::protocol::capability_light_gate | rig::protocol::capability_rgb |
    rig::protocol::capability_audio | rig::protocol::capability_valve |
    rig::protocol::capability_ir | rig::protocol::capability_sync_capture;

Node *find_address(std::uint8_t address) {
  for (Node &node : nodes) {
    if (node.present && node.address == address) {
      return &node;
    }
  }
  return nullptr;
}

/* Called with nodes_mutex held whenever a slot starts answering, or its
 * board restarts: the node's clock and record queue start again. */
void restart_link_state(Node &node) {
  node.queue_drained = false;
  node.have_pair = false;
  node.model.reset();
}

bool boot_record_uuid(const rig::protocol::RecordView &record,
                      rig::protocol::DeviceUuid &uuid) {
  rig::protocol::TlvReader reader(record.payload, record.header.payload_length);
  rig::protocol::FieldView field;
  while (reader.next(field)) {
    if (field.tag == static_cast<std::uint16_t>(rig::protocol::Field::uuid) &&
        field.length == uuid.size()) {
      std::memcpy(uuid.data(), field.value, uuid.size());
      return true;
    }
  }
  return false;
}

void publish_record(std::uint8_t source, rig::protocol::RecordType type,
                    std::uint16_t flags, std::uint32_t record_id,
                    std::uint32_t session_id, std::uint64_t ticks,
                    const std::uint8_t *payload, std::size_t payload_length,
                    bool diagnostic_priority = false) {
  rig::protocol::RecordHeader header;
  header.type = type;
  header.flags = flags;
  header.record_id = record_id;
  header.session_id = session_id;
  header.timestamp_ticks = ticks;
  header.clock_hz = rig::config::common_timer_hz;
  std::uint8_t record[rig::protocol::max_record_size]{};
  const std::size_t length = rig::protocol::encode_record(
      header, payload, payload_length, record, sizeof(record));
  if (length != 0U) {
    (void)hub_ble::send_to_host(source, rig::transport::Channel::record,
                                     record, length, 0, 0, 0,
                                     diagnostic_priority);
  }
}

void publish_quality(Node &node, std::uint64_t now_ticks) {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  if (!node.present ||
      current_telemetry == rig::protocol::TelemetryLevel::critical) {
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
  rig::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.i64(static_cast<std::uint16_t>(rig::protocol::Field::rms_ns),
                   static_cast<std::int64_t>(quality.rms_ticks * 62.5));
  (void)fields.i64(static_cast<std::uint16_t>(rig::protocol::Field::skew_ppb),
                   quality.skew_ppb);
  (void)fields.u8(
      static_cast<std::uint16_t>(rig::protocol::Field::model_points),
      static_cast<std::uint8_t>(quality.points));
  (void)fields.u32(
      static_cast<std::uint16_t>(rig::protocol::Field::model_generation),
      quality.generation);
  (void)fields.u32(
      static_cast<std::uint16_t>(rig::protocol::Field::transport_errors),
      errors);
  const std::uint16_t flags =
      quality.valid ? rig::protocol::synchronized : 0U;
  publish_record(address, rig::protocol::RecordType::sync_quality, flags,
                 quality.generation, 0, now_ticks, payload, fields.size());
}

void publish_inventory(const Node &node) {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  const rig::protocol::DeviceUuid uuid = node.uuid;
  const std::uint8_t address = node.address;
  const std::uint8_t logical_slot = node.logical_slot;
  const std::uint32_t capabilities = node.capabilities;
  const bool present = node.present;
  k_mutex_unlock(&nodes_mutex);
  std::uint8_t payload[64]{};
  rig::protocol::TlvWriter fields(payload, sizeof(payload));
  (void)fields.bytes(static_cast<std::uint16_t>(rig::protocol::Field::uuid),
                     uuid.data(), static_cast<std::uint8_t>(uuid.size()));
  (void)fields.u8(
      static_cast<std::uint16_t>(rig::protocol::Field::link_address),
      address);
  (void)fields.u8(
      static_cast<std::uint16_t>(rig::protocol::Field::logical_slot),
      logical_slot);
  (void)fields.u32(static_cast<std::uint16_t>(rig::protocol::Field::value),
                   capabilities);
  (void)fields.boolean(
      static_cast<std::uint16_t>(rig::protocol::Field::state), present);
  publish_record(rig::config::hub_address,
                 rig::protocol::RecordType::inventory,
                 rig::protocol::critical, 0, 0, hub_time::now(), payload,
                 fields.size(), true);
}

void process_sync(Node &node, const rig::protocol::RecordView &record) {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  if (!node.queue_drained) {
    k_mutex_unlock(&nodes_mutex);
    return;
  }
  const std::uint32_t current_pulse = hub_time::pulse_count();
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

  const std::uint64_t hub_ticks = hub_time::pulse_ticks(paired_pulse);
  if (!node.model.add(record.header.timestamp_ticks, hub_ticks)) {
    k_mutex_unlock(&nodes_mutex);
    return;
  }
  node.have_pair = true;
  node.last_pulse = paired_pulse;
  node.last_local = record.header.timestamp_ticks;

  const bool publish_pair =
      current_telemetry == rig::protocol::TelemetryLevel::full;
  const std::uint8_t address = node.address;
  k_mutex_unlock(&nodes_mutex);
  if (publish_pair) {
    std::uint8_t payload[56]{};
    rig::protocol::TlvWriter fields(payload, sizeof(payload));
    (void)fields.u32(
        static_cast<std::uint16_t>(rig::protocol::Field::sequence),
        paired_pulse);
    (void)fields.u64(
        static_cast<std::uint16_t>(rig::protocol::Field::requested_ticks),
        hub_ticks);
    (void)fields.u64(
        static_cast<std::uint16_t>(rig::protocol::Field::actual_ticks),
        record.header.timestamp_ticks);
    publish_record(address, rig::protocol::RecordType::clock_pair,
                   rig::protocol::synchronized, record.header.record_id,
                   record.header.session_id, hub_ticks, payload, fields.size());
  }
}

void forward(Node &node, const rig::protocol::RecordView &record,
             const std::uint8_t *raw, std::size_t raw_length) {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  const rig::protocol::TelemetryLevel level = current_telemetry;
  const std::uint8_t address = node.address;
  if (!rig::protocol::record_visible_at(record.header.type, level)) {
    k_mutex_unlock(&nodes_mutex);
    return;
  }

  const bool event =
      record.header.type == rig::protocol::RecordType::light_gate ||
      record.header.type == rig::protocol::RecordType::digital_input ||
      record.header.type == rig::protocol::RecordType::output_change ||
      record.header.type == rig::protocol::RecordType::command_result;
  if (!event) {
    k_mutex_unlock(&nodes_mutex);
    (void)hub_ble::send_to_host(
        address, rig::transport::Channel::record, raw, raw_length, 0, 0, 0,
        (record.header.flags & rig::protocol::critical) != 0U);
    return;
  }

  std::uint8_t payload[rig::protocol::record_max_payload]{};
  std::size_t payload_length = record.header.payload_length;
  std::memcpy(payload, record.payload, payload_length);
  std::uint64_t converted{};
  std::uint16_t flags = record.header.flags;
  if (node.model.predict(record.header.timestamp_ticks, converted)) {
    rig::protocol::TlvWriter extra(payload + payload_length,
                                     sizeof(payload) - payload_length);
    (void)extra.u64(
        static_cast<std::uint16_t>(rig::protocol::Field::reference_ticks),
        converted);
    payload_length += extra.size();
    flags |= rig::protocol::synchronized;
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
  const std::uint8_t slot = node.logical_slot;
  k_mutex_unlock(&nodes_mutex);
  hub_rs485::Received response;
  const int error = hub_rs485::exchange(
      address, rig::transport::Channel::record, nullptr, 0, response, 12,
      rig::transport::ack_required, true);
  if (error != 0) {
    k_mutex_lock(&nodes_mutex, K_FOREVER);
    // An empty slot is not an error, only a lost board is.
    if (was_present) {
      ++node.transport_errors;
    }
    node.next_poll_ms =
        now_ms + (was_present ? assigned_poll_interval_ms
                              : absent_probe_interval_ms);
    bool became_absent = false;
    if (was_present &&
        static_cast<std::uint32_t>(k_uptime_get()) - node.last_seen_ms >
            absent_after_ms) {
      node.present = false;
      node.model.reset();
      became_absent = true;
    }
    k_mutex_unlock(&nodes_mutex);
    if (became_absent) {
      hub_debug::log("NODE_ABSENT node=%u address=0x%02x\r\n",
                     static_cast<unsigned>(slot),
                     static_cast<unsigned>(address));
      publish_inventory(node);
      hub_control::dependency_disconnected("Node disconnected");
    }
    return;
  }
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  node.present = true;
  node.next_poll_ms = now_ms + assigned_poll_interval_ms;
  node.last_seen_ms = static_cast<std::uint32_t>(k_uptime_get());
  if (!was_present) {
    restart_link_state(node);
  }
  k_mutex_unlock(&nodes_mutex);
  if (!was_present) {
    hub_debug::log("NODE_PRESENT node=%u address=0x%02x\r\n",
                   static_cast<unsigned>(slot),
                   static_cast<unsigned>(address));
    publish_inventory(node);
  }
  if (response.length == 0U) {
    k_mutex_lock(&nodes_mutex, K_FOREVER);
    node.queue_drained = true;
    k_mutex_unlock(&nodes_mutex);
    return;
  }

  rig::protocol::RecordView record;
  if (!rig::protocol::decode_record(response.payload.data(), response.length,
                                      record)) {
    k_mutex_lock(&nodes_mutex, K_FOREVER);
    ++node.transport_errors;
    k_mutex_unlock(&nodes_mutex);
    return;
  }
  if (record.header.type == rig::protocol::RecordType::boot) {
    /*
     * The board reset. With discovery, a reset board dropped off the bus
     * and was re-assigned; now it answers again at once at the same
     * address, so the reset has to be noticed here. Its clock restarted and
     * it has left any session, so the model starts again and a running
     * session is stopped, as it was when the board went missing.
     */
    rig::protocol::DeviceUuid uuid{};
    const bool have_uuid = boot_record_uuid(record, uuid);
    k_mutex_lock(&nodes_mutex, K_FOREVER);
    restart_link_state(node);
    if (have_uuid) {
      node.uuid = uuid;
    }
    k_mutex_unlock(&nodes_mutex);
    hub_debug::log("NODE_BOOT node=%u uuid=%02x%02x%02x%02x...\r\n",
                   static_cast<unsigned>(slot), uuid[0], uuid[1], uuid[2],
                   uuid[3]);
    publish_inventory(node);
    if (was_present) {
      hub_control::dependency_disconnected("Node restarted");
    }
  }
  if (record.header.type == rig::protocol::RecordType::sync_observation) {
    process_sync(node, record);
  }
  forward(node, record, response.payload.data(), response.length);
}

void command_error(const Command &command, rig::protocol::Status status) {
  rig::protocol::CommandMessageView request;
  if (!rig::protocol::decode_command(command.bytes.data(), command.length,
                                      request)) {
    return;
  }
  rig::protocol::MessageHeader header = request.header;
  header.kind = rig::protocol::MessageKind::response;
  header.status = status;
  header.flags = 0;
  std::uint8_t response[rig::protocol::command_max_message_size]{};
  const std::size_t length = rig::protocol::encode_command(
      header, nullptr, 0, response, sizeof(response));
  (void)hub_ble::send_to_host(
      rig::config::hub_address, rig::transport::Channel::command,
      response, length, command.host_transfer, rig::transport::response);
}

void handle_command(const Command &command) {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  Node *node = find_address(command.destination);
  if (node == nullptr) {
    k_mutex_unlock(&nodes_mutex);
    command_error(command, rig::protocol::Status::transport_error);
    return;
  }
  const std::uint8_t address = node->address;
  k_mutex_unlock(&nodes_mutex);

  hub_rs485::Received response;
  const int error =
      hub_rs485::exchange(address, rig::transport::Channel::command,
                             command.bytes.data(), command.length, response, 8);
  if (error != 0 || response.channel != rig::transport::Channel::command) {
    command_error(command, rig::protocol::Status::transport_error);
    return;
  }
  (void)hub_ble::send_to_host(
      address, rig::transport::Channel::command, response.payload.data(),
      response.length, command.host_transfer, rig::transport::response);
}

/* Tells every node the hub has restarted, so they make their outputs safe.
 * Nodes keep their flashed addresses; the nonce is unused. */
void release_all() {
  rig::protocol::DiscoveryHeader header;
  header.type = rig::protocol::DiscoveryType::release;
  header.nonce = 0;
  std::uint8_t message[16]{};
  const std::size_t length = rig::protocol::encode_discovery(
      header, nullptr, 0, message, sizeof(message));
  (void)hub_rs485::send_one_way(rig::config::broadcast_address,
                                   rig::transport::Channel::discovery, 1, 0,
                                   message, length);
  k_sleep(K_MSEC(20));
}

void manager_thread(void *, void *, void *) {
  k_sem_take(&start_sem, K_FOREVER);
  release_all();

  /*
   * Presence is the poll loop itself: every slot is asked "are you there?"
   * on each pass. Present slots are polled every 10 ms; absent ones every
   * 500 ms, so the order boards and hub power up in does not matter.
   */
  while (true) {
    Command command;
    while (k_msgq_get(&commands, &command, K_NO_WAIT) == 0) {
      handle_command(command);
    }

    for (Node &node : nodes) {
      poll(node);
      publish_quality(node, hub_time::now());
    }
    k_sleep(K_MSEC(1));
  }
}

K_THREAD_DEFINE(manager_thread_id, 4096, manager_thread, nullptr, nullptr,
                nullptr, 7, 0, 0);

} // namespace

int initialize() {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  for (std::size_t slot = 0; slot < nodes.size(); ++slot) {
    Node &node = nodes[slot];
    node = {};
    node.address = rig::config::node_address(slot);
    node.logical_slot = static_cast<std::uint8_t>(slot);
    node.capabilities = node_capabilities;
  }
  current_telemetry = rig::protocol::TelemetryLevel::standard;
  k_mutex_unlock(&nodes_mutex);
  k_sem_give(&start_sem);
  return 0;
}

std::size_t count() {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  std::size_t result = 0;
  for (const Node &node : nodes) {
    result += node.present ? 1U : 0U;
  }
  k_mutex_unlock(&nodes_mutex);
  return result;
}

std::size_t inventory_bytes(std::uint8_t *destination, std::size_t capacity) {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  std::size_t present_count = 0;
  for (const Node &node : nodes) {
    present_count += node.present ? 1U : 0U;
  }
  const std::size_t needed = present_count * 18U;
  if (destination == nullptr || capacity < needed) {
    k_mutex_unlock(&nodes_mutex);
    return 0;
  }
  // 18 bytes per present node: UUID (zero until its boot record has been
  // seen), address, node number, capabilities.
  std::size_t offset = 0;
  for (const Node &node : nodes) {
    if (!node.present) {
      continue;
    }
    std::memcpy(destination + offset, node.uuid.data(), node.uuid.size());
    destination[offset + 12] = node.address;
    destination[offset + 13] = node.logical_slot;
    rig::wire::put_u32(destination + offset + 14, node.capabilities);
    offset += 18;
  }
  k_mutex_unlock(&nodes_mutex);
  return needed;
}

bool queue_command(std::uint8_t destination, const std::uint8_t *message,
                   std::size_t length, std::uint16_t host_transfer_id) {
  if (message == nullptr || length > rig::protocol::command_max_message_size) {
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
  for (const Node &node : nodes) {
    if (node.present) {
      (void)queue_command(node.address, message, length, 0);
    }
  }
  k_mutex_unlock(&nodes_mutex);
}

bool local_to_hub(std::uint8_t address, std::uint64_t local_ticks,
                     std::uint64_t &hub_ticks) {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  Node *node = find_address(address);
  const bool result =
      node != nullptr && node->model.predict(local_ticks, hub_ticks);
  k_mutex_unlock(&nodes_mutex);
  return result;
}

void set_telemetry(rig::protocol::TelemetryLevel level) {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  current_telemetry = level;
  k_mutex_unlock(&nodes_mutex);
}

rig::protocol::TelemetryLevel telemetry() {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  const auto result = current_telemetry;
  k_mutex_unlock(&nodes_mutex);
  return result;
}

bool snapshot(std::size_t index, NodeSnapshot &output) {
  k_mutex_lock(&nodes_mutex, K_FOREVER);
  const Node *selected = nullptr;
  std::size_t visible_index = 0;
  for (std::size_t raw_index = 0; raw_index < nodes.size(); ++raw_index) {
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

} // namespace hub_nodes
