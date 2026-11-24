#include "rig_protocol/discovery_protocol.hpp"

#include <cstring>

#include "rig_protocol/bytes.hpp"
#include "rig_protocol/crc16.hpp"

namespace rig::protocol {

std::size_t encode_discovery(const DiscoveryHeader &header,
                            const std::uint8_t *payload,
                            std::size_t payload_length,
                            std::uint8_t *destination, std::size_t capacity) {
  if (destination == nullptr || payload_length > 256U ||
      capacity < discovery_header_size + payload_length ||
      (payload_length != 0U && payload == nullptr)) {
    return 0;
  }

  std::memset(destination, 0, discovery_header_size);
  wire::put_u16(destination, discovery_magic);
  destination[2] = discovery_version;
  destination[3] = static_cast<std::uint8_t>(header.type);
  wire::put_u32(destination + 4, header.nonce);
  wire::put_u16(destination + 8, static_cast<std::uint16_t>(payload_length));
  if (payload_length != 0U) {
    std::memcpy(destination + discovery_header_size, payload, payload_length);
  }
  return discovery_header_size + payload_length;
}

bool decode_discovery(const std::uint8_t *message, std::size_t length,
                     DiscoveryView &output) {
  if (message == nullptr || length < discovery_header_size ||
      wire::get_u16(message) != discovery_magic ||
      message[2] != discovery_version) {
    return false;
  }
  const std::uint16_t payload_length = wire::get_u16(message + 8);
  if (length != discovery_header_size + payload_length ||
      wire::get_u16(message + 10) != 0U) {
    return false;
  }
  output.header.type = static_cast<DiscoveryType>(message[3]);
  output.header.nonce = wire::get_u32(message + 4);
  output.header.payload_length = payload_length;
  output.payload = message + discovery_header_size;
  return true;
}

std::uint8_t discovery_slot(const DeviceUuid &uuid, std::uint32_t nonce,
                            std::uint8_t slot_count) {
  if (slot_count == 0U) {
    return 0U;
  }
  std::uint8_t input[uuid_size + 4];
  std::memcpy(input, uuid.data(), uuid.size());
  wire::put_u32(input + uuid.size(), nonce);
  return static_cast<std::uint8_t>(wire::crc16_ccitt(input, sizeof(input)) %
                                   slot_count);
}

std::size_t encode_offer(const Offer &offer, std::uint8_t *destination,
                         std::size_t capacity) {
  constexpr std::size_t size = uuid_size + 8U;
  if (destination == nullptr || capacity < size) {
    return 0;
  }
  std::memcpy(destination, offer.uuid.data(), offer.uuid.size());
  wire::put_u32(destination + uuid_size, offer.capabilities);
  wire::put_u32(destination + uuid_size + 4U, offer.boot_count);
  return size;
}

bool decode_offer(const std::uint8_t *payload, std::size_t length,
                  Offer &output) {
  if (payload == nullptr || length != uuid_size + 8U) {
    return false;
  }
  std::memcpy(output.uuid.data(), payload, output.uuid.size());
  output.capabilities = wire::get_u32(payload + uuid_size);
  output.boot_count = wire::get_u32(payload + uuid_size + 4U);
  return true;
}

std::size_t encode_assignment(const Assignment &assignment,
                              std::uint8_t *destination, std::size_t capacity) {
  constexpr std::size_t size = uuid_size + 2U;
  if (destination == nullptr || capacity < size) {
    return 0;
  }
  std::memcpy(destination, assignment.uuid.data(), assignment.uuid.size());
  destination[uuid_size] = assignment.address;
  destination[uuid_size + 1U] = assignment.logical_slot;
  return size;
}

bool decode_assignment(const std::uint8_t *payload, std::size_t length,
                       Assignment &output) {
  if (payload == nullptr || length != uuid_size + 2U) {
    return false;
  }
  std::memcpy(output.uuid.data(), payload, output.uuid.size());
  output.address = payload[uuid_size];
  output.logical_slot = payload[uuid_size + 1U];
  return true;
}

} // namespace rig::protocol
