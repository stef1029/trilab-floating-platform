#include "fairy_shared/adelie_protocol.hpp"

#include <cstring>

#include "fairy_shared/bytes.hpp"

namespace fairy::protocol {

std::size_t encode_adelie(const MessageHeader &header,
                          const std::uint8_t *payload,
                          std::size_t payload_length, std::uint8_t *destination,
                          std::size_t capacity) {
  if (destination == nullptr || payload_length > adelie_max_payload ||
      capacity < adelie_header_size + payload_length ||
      (payload_length != 0U && payload == nullptr)) {
    return 0;
  }

  std::memset(destination, 0, adelie_header_size);
  wire::put_u16(destination, adelie_magic);
  destination[2] = adelie_version;
  destination[3] = static_cast<std::uint8_t>(adelie_header_size);
  destination[4] = static_cast<std::uint8_t>(header.kind);
  destination[5] = static_cast<std::uint8_t>(header.status);
  wire::put_u16(destination + 6, static_cast<std::uint16_t>(header.opcode));
  wire::put_u16(destination + 8, header.flags);
  wire::put_u32(destination + 10, header.command_id);
  wire::put_u32(destination + 14, header.session_id);
  wire::put_u64(destination + 18, header.execute_at_ticks);
  wire::put_u32(destination + 26, header.deadline_ms);
  wire::put_u16(destination + 30, static_cast<std::uint16_t>(payload_length));

  if (payload_length != 0U) {
    std::memcpy(destination + adelie_header_size, payload, payload_length);
  }
  return adelie_header_size + payload_length;
}

bool decode_adelie(const std::uint8_t *message, std::size_t length,
                   AdelieMessageView &output) {
  if (message == nullptr || length < adelie_header_size ||
      wire::get_u16(message) != adelie_magic || message[2] != adelie_version ||
      message[3] != adelie_header_size) {
    return false;
  }

  const std::uint16_t payload_length = wire::get_u16(message + 30);
  if (payload_length > adelie_max_payload ||
      length != adelie_header_size + payload_length ||
      message[4] < static_cast<std::uint8_t>(MessageKind::command) ||
      message[4] > static_cast<std::uint8_t>(MessageKind::response)) {
    return false;
  }

  output.header.kind = static_cast<MessageKind>(message[4]);
  output.header.status = static_cast<Status>(message[5]);
  output.header.opcode = static_cast<Opcode>(wire::get_u16(message + 6));
  output.header.flags = wire::get_u16(message + 8);
  output.header.command_id = wire::get_u32(message + 10);
  output.header.session_id = wire::get_u32(message + 14);
  output.header.execute_at_ticks = wire::get_u64(message + 18);
  output.header.deadline_ms = wire::get_u32(message + 26);
  output.header.payload_length = payload_length;
  output.payload = message + adelie_header_size;
  return true;
}

} // namespace fairy::protocol
