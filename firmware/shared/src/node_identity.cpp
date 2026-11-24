#include "rig_protocol/node_identity.hpp"

#include <cstring>

#include "rig_protocol/bytes.hpp"
#include "rig_protocol/crc16.hpp"
#include "rig_protocol/system_config.hpp"

namespace rig::identity {

namespace {
inline constexpr std::size_t crc_offset = record_size - 2U;
} // namespace

std::size_t encode_identity(std::uint8_t node_id, std::uint8_t *destination,
                            std::size_t capacity) {
  if (destination == nullptr || capacity < record_size ||
      node_id >= rig::config::max_nodes) {
    return 0;
  }
  std::memset(destination, 0, record_size);
  wire::put_u32(destination, magic);
  destination[4] = version;
  destination[5] = node_id;
  destination[6] = static_cast<std::uint8_t>(~node_id);
  wire::put_u16(destination + crc_offset,
                wire::crc16_ccitt(destination, crc_offset));
  return record_size;
}

bool decode_identity(const std::uint8_t *record, std::size_t length,
                     std::uint8_t &node_id) {
  if (record == nullptr || length < record_size ||
      wire::get_u32(record) != magic || record[4] != version ||
      record[6] != static_cast<std::uint8_t>(~record[5]) ||
      wire::get_u16(record + crc_offset) !=
          wire::crc16_ccitt(record, crc_offset) ||
      record[5] >= rig::config::max_nodes) {
    return false;
  }
  node_id = record[5];
  return true;
}

} // namespace rig::identity
