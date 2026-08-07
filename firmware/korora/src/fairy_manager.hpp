#pragma once

#include <cstddef>
#include <cstdint>

#include "fairy_shared/adelie_protocol.hpp"
#include "fairy_shared/clock_model.hpp"
#include "fairy_shared/fairy_protocol.hpp"
#include "fairy_shared/magellan_protocol.hpp"

namespace korora_fairies {

struct NodeSnapshot {
  fairy::protocol::DeviceUuid uuid{};
  std::uint8_t address{};
  std::uint8_t logical_slot{0xFF};
  std::uint32_t capabilities{};
  bool present{};
  bool synchronized{};
  fairy::time::ClockQuality clock{};
  std::uint32_t transport_errors{};
  std::uint32_t last_seen_ms{};
};

int initialize();
std::size_t count();
std::size_t inventory_bytes(std::uint8_t *destination, std::size_t capacity);

fairy::protocol::Status apply_inventory(const std::uint8_t *packed,
                                        std::size_t length);

bool queue_command(std::uint8_t destination, const std::uint8_t *message,
                   std::size_t length, std::uint16_t host_transfer_id);
void queue_command_for_all(const std::uint8_t *message, std::size_t length);

bool local_to_korora(std::uint8_t address, std::uint64_t local_ticks,
                     std::uint64_t &korora_ticks);

void set_telemetry(fairy::protocol::TelemetryLevel level);
fairy::protocol::TelemetryLevel telemetry();
bool snapshot(std::size_t index, NodeSnapshot &output);

} // namespace korora_fairies
