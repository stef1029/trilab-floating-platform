#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "fairy_shared/fairy_protocol.hpp"
#include "fairy_shared/static_queue.hpp"

namespace fairy_records {

struct StoredRecord {
  std::array<std::uint8_t, fairy::protocol::fairy_max_record_size> bytes{};
  std::uint16_t length{};
};

void initialize();
bool enqueue(fairy::protocol::RecordType type, std::uint16_t flags,
             std::uint64_t timestamp_ticks, std::uint32_t session_id,
             const std::uint8_t *payload = nullptr,
             std::size_t payload_length = 0);
const StoredRecord *front();
bool pop();
std::size_t size();
std::uint32_t dropped();
std::uint32_t next_id();

} // namespace fairy_records
