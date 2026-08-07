#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fairy::transport {

inline constexpr std::uint16_t magic = 0xA7C3;
inline constexpr std::uint8_t version = 1;
inline constexpr std::size_t header_size = 12;
inline constexpr std::size_t crc_size = 2;
inline constexpr std::size_t overhead = header_size + crc_size;
inline constexpr std::size_t max_raw_frame_size = 255;
inline constexpr std::size_t max_fragment_payload =
    max_raw_frame_size - overhead;
inline constexpr std::size_t max_message_size = 512;
inline constexpr std::size_t max_encoded_uart_frame = 258;

enum class Channel : std::uint8_t {
  fairy = 1,
  adelie = 2,
  magellan = 3,
};

enum Flag : std::uint8_t {
  ack_required = 1U << 0U,
  response = 1U << 1U,
  acknowledgement = 1U << 2U,
  error = 1U << 3U,
  first_fragment = 1U << 4U,
  last_fragment = 1U << 5U,
};

struct Header {
  Channel channel{Channel::fairy};
  std::uint8_t flags{};
  std::uint8_t source{};
  std::uint8_t destination{};
  std::uint8_t fragment_index{};
  std::uint8_t fragment_count{1};
  std::uint8_t payload_length{};
  std::uint16_t transfer_id{};
};

struct FrameView {
  Header header{};
  const std::uint8_t *payload{};
  std::size_t payload_length{};
};

struct MessageView {
  Channel channel{Channel::fairy};
  std::uint8_t flags{};
  std::uint8_t source{};
  std::uint8_t destination{};
  std::uint16_t transfer_id{};
  const std::uint8_t *payload{};
  std::size_t payload_length{};
};

enum class DecodeResult {
  ok,
  incomplete,
  bad_length,
  bad_magic,
  bad_version,
  bad_channel,
  bad_fragment,
  bad_crc,
  overflow,
  out_of_order,
};

std::size_t encode_frame(const Header &header, const std::uint8_t *payload,
                         std::size_t payload_length, std::uint8_t *destination,
                         std::size_t capacity);

DecodeResult decode_frame(const std::uint8_t *frame, std::size_t length,
                          FrameView &output);

std::size_t cobs_encode(const std::uint8_t *source, std::size_t length,
                        std::uint8_t *destination, std::size_t capacity);

std::size_t cobs_decode(const std::uint8_t *source, std::size_t length,
                        std::uint8_t *destination, std::size_t capacity);

class UartDecoder {
public:
  DecodeResult push(std::uint8_t byte, FrameView &output);
  void reset();
  std::uint32_t overflow_count() const { return overflow_count_; }
  std::uint32_t malformed_count() const { return malformed_count_; }

private:
  std::array<std::uint8_t, max_encoded_uart_frame> encoded_{};
  std::array<std::uint8_t, max_raw_frame_size> decoded_{};
  std::size_t length_{};
  bool overflowed_{};
  std::uint32_t overflow_count_{};
  std::uint32_t malformed_count_{};
};

class Reassembler {
public:
  DecodeResult accept(const FrameView &frame, MessageView &output);
  void reset();

private:
  std::array<std::uint8_t, max_message_size> message_{};
  std::size_t length_{};
  std::uint16_t transfer_id_{};
  std::uint8_t next_fragment_{};
  std::uint8_t fragment_count_{};
  Header first_header_{};
  bool active_{};
};

} // namespace fairy::transport
