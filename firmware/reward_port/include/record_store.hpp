#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "rig_protocol/record_protocol.hpp"
#include "rig_protocol/static_queue.hpp"

namespace node_records {

struct StoredRecord {
  std::array<std::uint8_t, rig::protocol::max_record_size> bytes{};
  std::uint16_t length{};
};

void initialize();
bool enqueue(rig::protocol::RecordType type, std::uint16_t flags,
             std::uint64_t timestamp_ticks, std::uint32_t session_id,
             const std::uint8_t *payload = nullptr,
             std::size_t payload_length = 0);
const StoredRecord *front();
bool pop();
std::size_t size();
std::uint32_t dropped();
std::uint32_t next_id();

} // namespace node_records
