#pragma once

#include <cstddef>
#include <cstdint>

namespace fairy::protocol {

inline constexpr std::uint16_t adelie_magic = 0xAD1E;
inline constexpr std::uint8_t adelie_version = 2;
inline constexpr std::size_t adelie_header_size = 32;
inline constexpr std::size_t adelie_max_payload = 256;
inline constexpr std::size_t adelie_max_message_size =
    adelie_header_size + adelie_max_payload;

enum class MessageKind : std::uint8_t {
  command = 1,
  response = 2,
};

enum class Status : std::uint8_t {
  ok = 0,
  accepted = 1,
  busy = 2,
  bad_message = 3,
  unsupported = 4,
  invalid_state = 5,
  invalid_parameter = 6,
  not_synchronized = 7,
  inventory_mismatch = 8,
  timeout = 9,
  transport_error = 10,
  queue_full = 11,
  duplicate = 12,
  safety_lock = 13,
  session_mismatch = 14,
  internal_error = 15,
};

enum class Opcode : std::uint16_t {
  ping = 0x0001,
  get_inventory = 0x0002,
  apply_inventory = 0x0003,
  identify = 0x0004,
  get_health = 0x0005,
  clear_faults = 0x0006,
  set_telemetry = 0x0007,
  clock_exchange = 0x0008,

  start_session = 0x0100,
  stop_session = 0x0101,

  set_rgb = 0x0200,
  set_ir = 0x0201,
  set_audio = 0x0202,
  actuate_valve = 0x0203,
  configure_valve = 0x0204,
  configure_pins = 0x0205,
  set_illumination = 0x0206,
  set_status_led = 0x0207,

  schedule_ttl = 0x0300,
  start_ttl_train = 0x0301,
  stop_ttl_train = 0x0302,

  start_sync_test = 0x0400,
  stop_sync_test = 0x0401,

  scry_stream = 0x0500,
  scry_tare = 0x0501,
  scry_calibration = 0x0502,
};

enum CommandFlag : std::uint16_t {
  require_response = 1U << 0U,
  execute_immediately = 1U << 1U,
  replace_existing = 1U << 2U,
  safety_authorized = 1U << 3U,
};

enum class CommandField : std::uint16_t {
  telemetry_level = 0x1000,
  uuid_list = 0x1001,
  logical_slot = 0x1002,
  identify_duration_ms = 0x1003,
  red = 0x1010,
  green = 0x1011,
  blue = 0x1012,
  audio_mode = 0x1020,
  frequency_hz = 0x1021,
  low_frequency_hz = 0x1022,
  high_frequency_hz = 0x1023,
  amplitude = 0x1024,
  duration_ms = 0x1025,
  enabled = 0x1026,
  spike_duration_us = 0x1030,
  spike_duty_per_mille = 0x1031,
  hold_duty_per_mille = 0x1032,
  max_on_time_us = 0x1033,
  minimum_interval_us = 0x1034,
  vload_millivolts = 0x1035,
  ttl_frequency_millihz = 0x1040,
  ttl_width_us = 0x1041,
  ttl_count = 0x1042,
  sequence = 0x1043,
  test_command_interval_ms = 0x1050,
  clock_t1_ns = 0x1060,
  clock_t2_ticks = 0x1061,
  clock_t3_ticks = 0x1062,
  drive_mode = 0x1070,
  brightness = 0x1071,
  channel = 0x1072,
  led_index = 0x1073,
  sample_rate_hz = 0x1074,
  sample_count = 0x1075,
  scale = 0x1076,
};

struct MessageHeader {
  MessageKind kind{MessageKind::command};
  Status status{Status::ok};
  Opcode opcode{Opcode::ping};
  std::uint16_t flags{};
  std::uint32_t command_id{};
  std::uint32_t session_id{};
  std::uint64_t execute_at_ticks{};
  std::uint32_t deadline_ms{};
  std::uint16_t payload_length{};
};

struct AdelieMessageView {
  MessageHeader header{};
  const std::uint8_t *payload{};
};

std::size_t encode_adelie(const MessageHeader &header,
                          const std::uint8_t *payload,
                          std::size_t payload_length, std::uint8_t *destination,
                          std::size_t capacity);

bool decode_adelie(const std::uint8_t *message, std::size_t length,
                   AdelieMessageView &output);

} // namespace fairy::protocol
