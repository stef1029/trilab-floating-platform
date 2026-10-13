#include <cassert>
#include <cstdint>
#include <cstring>

#include "fairy_shared/adelie_protocol.hpp"
#include "fairy_shared/clock_model.hpp"
#include "fairy_shared/fairy_protocol.hpp"
#include "fairy_shared/magellan_protocol.hpp"
#include "fairy_shared/system_config.hpp"
#include "fairy_shared/tlv.hpp"
#include "fairy_shared/transport.hpp"

int main() {
  using namespace fairy;

  static_assert(config::max_fairies == 6);
  static_assert(config::necneven_address == config::korora_address);
  static_assert(config::hecate_address == config::adelie_address);
  static_assert(config::will_o_wisp_address == 0x20);
  static_assert(static_cast<std::uint16_t>(protocol::Opcode::set_illumination) ==
                0x0206);
  static_assert(static_cast<std::uint16_t>(protocol::Opcode::scry_stream) ==
                0x0500);
  static_assert(static_cast<std::uint16_t>(protocol::RecordType::scry_sample) ==
                0x0603);

  std::uint8_t tlv_bytes[64]{};
  protocol::TlvWriter tlv(tlv_bytes, sizeof(tlv_bytes));
  assert(tlv.u32(static_cast<std::uint16_t>(protocol::Field::sequence), 42));
  assert(
      tlv.string(static_cast<std::uint16_t>(protocol::Field::detail), "test"));

  protocol::RecordHeader record_header;
  record_header.type = protocol::RecordType::ttl_generated;
  record_header.flags = protocol::critical | protocol::actual_time;
  record_header.record_id = 7;
  record_header.session_id = 9;
  record_header.timestamp_ticks = 123456789;
  record_header.clock_hz = 16'000'000;

  std::uint8_t record[protocol::fairy_max_record_size]{};
  const std::size_t record_size = protocol::encode_record(
      record_header, tlv_bytes, tlv.size(), record, sizeof(record));
  assert(record_size != 0);

  protocol::RecordView record_view;
  assert(protocol::decode_record(record, record_size, record_view));
  assert(record_view.header.record_id == 7);
  assert(record_view.header.type == protocol::RecordType::ttl_generated);

  transport::Header link_header;
  link_header.channel = transport::Channel::fairy;
  link_header.flags = transport::first_fragment | transport::last_fragment;
  link_header.source = 0x10;
  link_header.destination = 0x03;
  link_header.transfer_id = 17;

  std::uint8_t frame[transport::max_raw_frame_size]{};
  const std::size_t frame_size = transport::encode_frame(
      link_header, record, record_size, frame, sizeof(frame));
  assert(frame_size != 0);

  transport::FrameView frame_view;
  assert(transport::decode_frame(frame, frame_size, frame_view) ==
         transport::DecodeResult::ok);
  assert(frame_view.header.source == 0x10);

  std::uint8_t cobs[transport::max_encoded_uart_frame]{};
  const std::size_t cobs_size =
      transport::cobs_encode(frame, frame_size, cobs, sizeof(cobs));
  assert(cobs_size != 0 && cobs[cobs_size - 1] == 0);

  transport::UartDecoder decoder;
  transport::FrameView uart_view;
  for (std::size_t i = 0; i + 1 < cobs_size; ++i) {
    assert(decoder.push(cobs[i], uart_view) ==
           transport::DecodeResult::incomplete);
  }
  assert(decoder.push(0, uart_view) == transport::DecodeResult::ok);
  assert(uart_view.payload_length == record_size);
  assert(std::memcmp(uart_view.payload, record, record_size) == 0);

  std::uint8_t maximum_raw[transport::max_raw_frame_size]{};
  std::memset(maximum_raw, 0xA5, sizeof(maximum_raw));
  std::uint8_t maximum_cobs[transport::max_encoded_uart_frame]{};
  const std::size_t maximum_cobs_size = transport::cobs_encode(
      maximum_raw, sizeof(maximum_raw), maximum_cobs, sizeof(maximum_cobs));
  assert(maximum_cobs_size != 0);
  assert(maximum_cobs[maximum_cobs_size - 1] == 0);
  std::uint8_t maximum_decoded[transport::max_raw_frame_size]{};
  const std::size_t maximum_decoded_size =
      transport::cobs_decode(maximum_cobs, maximum_cobs_size - 1U,
                             maximum_decoded, sizeof(maximum_decoded));
  assert(maximum_decoded_size == sizeof(maximum_raw));
  assert(std::memcmp(maximum_decoded, maximum_raw, sizeof(maximum_raw)) == 0);

  frame[frame_size - 1U] ^= 0x01U;
  assert(transport::decode_frame(frame, frame_size, frame_view) ==
         transport::DecodeResult::bad_crc);
  frame[frame_size - 1U] ^= 0x01U;

  transport::Reassembler reassembler;
  transport::MessageView reassembled;
  std::uint8_t fragment_a[transport::max_raw_frame_size]{};
  std::uint8_t fragment_b[transport::max_raw_frame_size]{};
  link_header.fragment_count = 2;
  link_header.fragment_index = 0;
  link_header.flags = transport::first_fragment;
  const std::size_t fragment_a_size = transport::encode_frame(
      link_header, record, 20, fragment_a, sizeof(fragment_a));
  link_header.fragment_index = 1;
  link_header.flags = transport::last_fragment;
  const std::size_t fragment_b_size =
      transport::encode_frame(link_header, record + 20, record_size - 20,
                              fragment_b, sizeof(fragment_b));
  transport::FrameView fragment_view;
  assert(transport::decode_frame(fragment_a, fragment_a_size, fragment_view) ==
         transport::DecodeResult::ok);
  assert(reassembler.accept(fragment_view, reassembled) ==
         transport::DecodeResult::incomplete);
  assert(transport::decode_frame(fragment_b, fragment_b_size, fragment_view) ==
         transport::DecodeResult::ok);
  assert(reassembler.accept(fragment_view, reassembled) ==
         transport::DecodeResult::ok);
  assert(reassembled.payload_length == record_size);
  assert(std::memcmp(reassembled.payload, record, record_size) == 0);

  transport::Reassembler out_of_order;
  assert(transport::decode_frame(fragment_a, fragment_a_size, fragment_view) ==
         transport::DecodeResult::ok);
  assert(out_of_order.accept(fragment_view, reassembled) ==
         transport::DecodeResult::incomplete);
  link_header.fragment_index = 1;
  link_header.fragment_count = 3;
  const std::size_t wrong_fragment_size = transport::encode_frame(
      link_header, record + 20, 1, fragment_b, sizeof(fragment_b));
  assert(transport::decode_frame(fragment_b, wrong_fragment_size,
                                 fragment_view) == transport::DecodeResult::ok);
  assert(out_of_order.accept(fragment_view, reassembled) ==
         transport::DecodeResult::out_of_order);

  protocol::MessageHeader command_header;
  command_header.kind = protocol::MessageKind::command;
  command_header.opcode = protocol::Opcode::set_rgb;
  command_header.command_id = 99;
  command_header.session_id = 5;

  std::uint8_t command[protocol::adelie_max_message_size]{};
  const std::size_t command_size = protocol::encode_adelie(
      command_header, tlv_bytes, tlv.size(), command, sizeof(command));
  protocol::AdelieMessageView command_view;
  assert(command_size != 0);
  assert(protocol::decode_adelie(command, command_size, command_view));
  assert(command_view.header.command_id == 99);
  assert(protocol::record_visible_at(protocol::RecordType::light_gate,
                                     protocol::TelemetryLevel::critical));
  assert(!protocol::record_visible_at(protocol::RecordType::sync_observation,
                                      protocol::TelemetryLevel::standard));
  assert(protocol::record_visible_at(protocol::RecordType::sync_observation,
                                     protocol::TelemetryLevel::full));

  protocol::DeviceUuid uuid{{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
  const auto slot_a = protocol::discovery_slot(uuid, 123, 16);
  const auto slot_b = protocol::discovery_slot(uuid, 123, 16);
  assert(slot_a == slot_b && slot_a < 16);

  protocol::Assignment assignment;
  assignment.uuid = uuid;
  assignment.address = 0x10;
  assignment.logical_slot = 0;
  std::uint8_t assignment_payload[16]{};
  const std::size_t assignment_size = protocol::encode_assignment(
      assignment, assignment_payload, sizeof(assignment_payload));
  protocol::MagellanHeader magellan_header;
  magellan_header.type = protocol::MagellanType::assign;
  magellan_header.nonce = 456;
  std::uint8_t magellan[48]{};
  const std::size_t magellan_size =
      protocol::encode_magellan(magellan_header, assignment_payload,
                                assignment_size, magellan, sizeof(magellan));
  protocol::MagellanView magellan_view;
  protocol::Assignment decoded_assignment;
  assert(protocol::decode_magellan(magellan, magellan_size, magellan_view));
  assert(protocol::decode_assignment(magellan_view.payload,
                                     magellan_view.header.payload_length,
                                     decoded_assignment));
  assert(decoded_assignment.uuid == uuid);
  assert(decoded_assignment.address == 0x10);

  time::AffineClockModel<8> clock;
  for (std::uint64_t index = 0; index < 8; ++index) {
    const std::uint64_t local = 10'000'000ULL + index * 4'000'000ULL;
    const std::uint64_t reference = local + 12'000ULL + index * 4ULL;
    assert(clock.add(local, reference));
  }
  assert(clock.quality().valid);
  std::uint64_t predicted{};
  std::uint64_t inverse{};
  assert(clock.predict(50'000'000ULL, predicted));
  assert(clock.inverse(predicted, inverse));
  assert(inverse >= 49'999'999ULL && inverse <= 50'000'001ULL);

  return 0;
}
