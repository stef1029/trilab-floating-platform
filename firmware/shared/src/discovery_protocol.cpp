#include "rig_protocol/discovery_protocol.hpp"

#include <cstring>

#include "rig_protocol/bytes.hpp"

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

} // namespace rig::protocol
