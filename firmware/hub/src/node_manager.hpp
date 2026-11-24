#pragma once

#include <cstddef>
#include <cstdint>

#include "rig_protocol/command_protocol.hpp"
#include "rig_protocol/clock_model.hpp"
#include "rig_protocol/record_protocol.hpp"
#include "rig_protocol/discovery_protocol.hpp"

namespace hub_nodes {

struct NodeSnapshot {
  rig::protocol::DeviceUuid uuid{};
  std::uint8_t address{};
  std::uint8_t logical_slot{0xFF};
  std::uint32_t capabilities{};
  bool present{};
  bool synchronized{};
  rig::time::ClockQuality clock{};
  std::uint32_t transport_errors{};
  std::uint32_t last_seen_ms{};
};

int initialize();
std::size_t count();
std::size_t inventory_bytes(std::uint8_t *destination, std::size_t capacity);

rig::protocol::Status apply_inventory(const std::uint8_t *packed,
                                        std::size_t length);

bool queue_command(std::uint8_t destination, const std::uint8_t *message,
                   std::size_t length, std::uint16_t host_transfer_id);
void queue_command_for_all(const std::uint8_t *message, std::size_t length);

bool local_to_hub(std::uint8_t address, std::uint64_t local_ticks,
                     std::uint64_t &hub_ticks);

void set_telemetry(rig::protocol::TelemetryLevel level);
rig::protocol::TelemetryLevel telemetry();
bool snapshot(std::size_t index, NodeSnapshot &output);

} // namespace hub_nodes
