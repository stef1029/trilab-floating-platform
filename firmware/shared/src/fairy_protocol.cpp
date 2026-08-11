#include "fairy_shared/fairy_protocol.hpp"

#include <cstring>

#include "fairy_shared/bytes.hpp"

namespace fairy::protocol {

std::size_t encode_record(const RecordHeader &header,
                          const std::uint8_t *payload,
                          std::size_t payload_length, std::uint8_t *destination,
                          std::size_t capacity) {
  if (destination == nullptr || payload_length > fairy_max_payload ||
      capacity < fairy_header_size + payload_length ||
      (payload_length != 0U && payload == nullptr)) {
    return 0;
  }

  std::memset(destination, 0, fairy_header_size);
  wire::put_u16(destination, fairy_magic);
  destination[2] = fairy_version;
  destination[3] = static_cast<std::uint8_t>(fairy_header_size);
  wire::put_u16(destination + 4, static_cast<std::uint16_t>(header.type));
  wire::put_u16(destination + 6, header.flags);
  wire::put_u32(destination + 8, header.record_id);
  wire::put_u32(destination + 12, header.session_id);
  wire::put_u64(destination + 16, header.timestamp_ticks);
  wire::put_u32(destination + 24, header.clock_hz);
  wire::put_u16(destination + 28, static_cast<std::uint16_t>(payload_length));

  if (payload_length != 0U) {
    std::memcpy(destination + fairy_header_size, payload, payload_length);
  }
  return fairy_header_size + payload_length;
}

bool decode_record(const std::uint8_t *message, std::size_t length,
                   RecordView &output) {
  if (message == nullptr || length < fairy_header_size ||
      wire::get_u16(message) != fairy_magic || message[2] != fairy_version ||
      message[3] != fairy_header_size) {
    return false;
  }

  const std::uint16_t payload_length = wire::get_u16(message + 28);
  if (payload_length > fairy_max_payload ||
      length != fairy_header_size + payload_length ||
      wire::get_u16(message + 30) != 0U) {
    return false;
  }

  output.header.type = static_cast<RecordType>(wire::get_u16(message + 4));
  output.header.flags = wire::get_u16(message + 6);
  output.header.record_id = wire::get_u32(message + 8);
  output.header.session_id = wire::get_u32(message + 12);
  output.header.timestamp_ticks = wire::get_u64(message + 16);
  output.header.clock_hz = wire::get_u32(message + 24);
  output.header.payload_length = payload_length;
  output.payload = message + fairy_header_size;
  return true;
}

bool record_visible_at(RecordType type, TelemetryLevel level) {
  switch (type) {
  case RecordType::digital_input:
  case RecordType::output_change:
  case RecordType::ttl_scheduled:
  case RecordType::ttl_generated:
  case RecordType::ttl_captured:
  case RecordType::ttl_result:
  case RecordType::light_gate:
  case RecordType::command_result:
  case RecordType::fault:
  case RecordType::test_marker:
  case RecordType::transport_timing:
    return true;

  case RecordType::boot:
  case RecordType::health:
  case RecordType::inventory:
  case RecordType::link_quality:
  case RecordType::sync_quality:
  case RecordType::clock_model_reset:
  case RecordType::local_sensors:
  case RecordType::power_status:
    return level >= TelemetryLevel::standard;

  case RecordType::sync_observation:
  case RecordType::imu_samples:
  case RecordType::clock_pair:
    return level >= TelemetryLevel::full;
  }
  return level >= TelemetryLevel::full;
}

} // namespace fairy::protocol
