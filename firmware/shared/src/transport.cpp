#include "fairy_shared/transport.hpp"

#include <cstring>

#include "fairy_shared/bytes.hpp"
#include "fairy_shared/crc16.hpp"

namespace fairy::transport {

std::size_t encode_frame(const Header &header, const std::uint8_t *payload,
                         std::size_t payload_length, std::uint8_t *destination,
                         std::size_t capacity) {
  if (destination == nullptr || payload_length > max_fragment_payload ||
      payload_length > 0xFFU || header.fragment_count == 0U ||
      header.fragment_index >= header.fragment_count ||
      capacity < overhead + payload_length ||
      (payload_length != 0U && payload == nullptr)) {
    return 0;
  }

  wire::put_u16(destination, magic);
  destination[2] = version;
  destination[3] = static_cast<std::uint8_t>(header.channel);
  destination[4] = header.flags;
  destination[5] = header.source;
  destination[6] = header.destination;
  destination[7] = header.fragment_index;
  destination[8] = header.fragment_count;
  destination[9] = static_cast<std::uint8_t>(payload_length);
  wire::put_u16(destination + 10, header.transfer_id);

  if (payload_length != 0U) {
    std::memcpy(destination + header_size, payload, payload_length);
  }

  const std::size_t without_crc = header_size + payload_length;
  wire::put_u16(destination + without_crc,
                wire::crc16_ccitt(destination, without_crc));
  return without_crc + crc_size;
}

DecodeResult decode_frame(const std::uint8_t *frame, std::size_t length,
                          FrameView &output) {
  if (frame == nullptr || length < overhead || length > max_raw_frame_size) {
    return DecodeResult::bad_length;
  }
  if (wire::get_u16(frame) != magic) {
    return DecodeResult::bad_magic;
  }
  if (frame[2] != version) {
    return DecodeResult::bad_version;
  }
  if (frame[3] < static_cast<std::uint8_t>(Channel::fairy) ||
      frame[3] > static_cast<std::uint8_t>(Channel::magellan)) {
    return DecodeResult::bad_channel;
  }

  const std::uint8_t payload_length = frame[9];
  if (length != overhead + payload_length) {
    return DecodeResult::bad_length;
  }

  const std::uint8_t fragment_index = frame[7];
  const std::uint8_t fragment_count = frame[8];
  if (fragment_count == 0U || fragment_index >= fragment_count) {
    return DecodeResult::bad_fragment;
  }

  const std::uint16_t expected = wire::get_u16(frame + length - crc_size);
  const std::uint16_t actual = wire::crc16_ccitt(frame, length - crc_size);
  if (expected != actual) {
    return DecodeResult::bad_crc;
  }

  output.header.channel = static_cast<Channel>(frame[3]);
  output.header.flags = frame[4];
  output.header.source = frame[5];
  output.header.destination = frame[6];
  output.header.fragment_index = fragment_index;
  output.header.fragment_count = fragment_count;
  output.header.payload_length = payload_length;
  output.header.transfer_id = wire::get_u16(frame + 10);
  output.payload = frame + header_size;
  output.payload_length = payload_length;
  return DecodeResult::ok;
}

std::size_t cobs_encode(const std::uint8_t *source, std::size_t length,
                        std::uint8_t *destination, std::size_t capacity) {
  if (source == nullptr || destination == nullptr || capacity < 2U) {
    return 0;
  }

  std::size_t read = 0;
  std::size_t write = 1;
  std::size_t code_position = 0;
  std::uint8_t code = 1;

  while (read < length) {
    if (source[read] == 0U) {
      if (code_position >= capacity) {
        return 0;
      }
      destination[code_position] = code;
      code_position = write++;
      code = 1;
      if (write > capacity) {
        return 0;
      }
      ++read;
      continue;
    }

    if (write >= capacity) {
      return 0;
    }
    destination[write++] = source[read++];
    ++code;

    if (code == 0xFFU) {
      destination[code_position] = code;
      code_position = write++;
      code = 1;
      if (write > capacity) {
        return 0;
      }
    }
  }

  if (code_position >= capacity || write >= capacity) {
    return 0;
  }
  destination[code_position] = code;
  destination[write++] = 0U;
  return write;
}

std::size_t cobs_decode(const std::uint8_t *source, std::size_t length,
                        std::uint8_t *destination, std::size_t capacity) {
  if (source == nullptr || destination == nullptr || length == 0U) {
    return 0;
  }

  std::size_t read = 0;
  std::size_t write = 0;

  while (read < length) {
    const std::uint8_t code = source[read++];
    if (code == 0U || read + static_cast<std::size_t>(code - 1U) > length) {
      return 0;
    }

    for (std::uint8_t i = 1; i < code; ++i) {
      if (write >= capacity) {
        return 0;
      }
      destination[write++] = source[read++];
    }

    if (code != 0xFFU && read < length) {
      if (write >= capacity) {
        return 0;
      }
      destination[write++] = 0U;
    }
  }
  return write;
}

DecodeResult UartDecoder::push(std::uint8_t byte, FrameView &output) {
  if (byte != 0U) {
    if (length_ >= encoded_.size()) {
      overflowed_ = true;
    } else {
      encoded_[length_++] = byte;
    }
    return DecodeResult::incomplete;
  }

  if (length_ == 0U) {
    overflowed_ = false;
    return DecodeResult::incomplete;
  }

  if (overflowed_) {
    reset();
    ++overflow_count_;
    return DecodeResult::overflow;
  }

  const std::size_t decoded_length =
      cobs_decode(encoded_.data(), length_, decoded_.data(), decoded_.size());
  length_ = 0;
  if (decoded_length == 0U) {
    ++malformed_count_;
    return DecodeResult::bad_length;
  }

  const DecodeResult result =
      decode_frame(decoded_.data(), decoded_length, output);
  if (result != DecodeResult::ok) {
    ++malformed_count_;
  }
  return result;
}

void UartDecoder::reset() {
  length_ = 0;
  overflowed_ = false;
}

DecodeResult Reassembler::accept(const FrameView &frame, MessageView &output) {
  const Header &header = frame.header;
  if (header.fragment_index == 0U) {
    reset();
    active_ = true;
    transfer_id_ = header.transfer_id;
    fragment_count_ = header.fragment_count;
    first_header_ = header;
  }

  if (!active_ || header.transfer_id != transfer_id_ ||
      header.fragment_count != fragment_count_ ||
      header.fragment_index != next_fragment_ ||
      header.channel != first_header_.channel ||
      header.source != first_header_.source ||
      header.destination != first_header_.destination) {
    reset();
    return DecodeResult::out_of_order;
  }

  if (length_ + frame.payload_length > message_.size()) {
    reset();
    return DecodeResult::overflow;
  }

  if (frame.payload_length != 0U) {
    std::memcpy(message_.data() + length_, frame.payload, frame.payload_length);
  }
  length_ += frame.payload_length;
  ++next_fragment_;

  if (next_fragment_ != fragment_count_) {
    return DecodeResult::incomplete;
  }

  output.channel = first_header_.channel;
  output.flags = first_header_.flags;
  output.source = first_header_.source;
  output.destination = first_header_.destination;
  output.transfer_id = transfer_id_;
  output.payload = message_.data();
  output.payload_length = length_;
  active_ = false;
  return DecodeResult::ok;
}

void Reassembler::reset() {
  length_ = 0;
  transfer_id_ = 0;
  next_fragment_ = 0;
  fragment_count_ = 0;
  first_header_ = {};
  active_ = false;
}

} // namespace fairy::transport
