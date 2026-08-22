#pragma once

#include <cstddef>
#include <cstdint>

namespace fairy::protocol {

inline constexpr std::uint16_t fairy_magic = 0xFA13;
inline constexpr std::uint8_t fairy_version = 3;
inline constexpr std::size_t fairy_header_size = 32;
inline constexpr std::size_t fairy_max_payload = 256;
inline constexpr std::size_t fairy_max_record_size =
    fairy_header_size + fairy_max_payload;

enum class RecordType : std::uint16_t {
  boot = 0x0001,
  health = 0x0002,
  inventory = 0x0003,
  link_quality = 0x0004,
  transport_timing = 0x0005,
  sync_observation = 0x0100,
  sync_quality = 0x0101,
  clock_model_reset = 0x0102,
  clock_pair = 0x0103,
  digital_input = 0x0200,
  output_change = 0x0201,
  ttl_scheduled = 0x0202,
  ttl_generated = 0x0203,
  ttl_captured = 0x0204,
  ttl_result = 0x0205,
  light_gate = 0x0206,
  command_result = 0x0300,
  fault = 0x0400,
  test_marker = 0x0500,
  local_sensors = 0x0600,
  imu_samples = 0x0601,
  power_status = 0x0602,
  scry_sample = 0x0603,
  illumination_status = 0x0604,
};

enum RecordFlag : std::uint16_t {
  critical = 1U << 0U,
  first_after_reset = 1U << 1U,
  synchronized = 1U << 2U,
  scheduled = 1U << 3U,
  actual_time = 1U << 4U,
  loss_latched = 1U << 5U,
};

enum class TelemetryLevel : std::uint8_t {
  critical = 0,
  standard = 1,
  full = 2,
};

enum class Field : std::uint16_t {
  uuid = 0x0001,
  logical_slot = 0x0002,
  link_address = 0x0003,
  board_kind = 0x0004,
  firmware_version = 0x0005,
  reset_cause = 0x0006,
  uptime_ms = 0x0007,
  queue_depth = 0x0008,
  queue_capacity = 0x0009,
  dropped_records = 0x000A,
  transport_errors = 0x000B,
  duplicate_frames = 0x000C,
  related_transfer_id = 0x000D,
  command_id = 0x0010,
  status = 0x0011,
  operation = 0x0012,
  requested_ticks = 0x0013,
  actual_ticks = 0x0014,
  duration_us = 0x0015,
  sequence = 0x0016,
  state = 0x0017,
  channel = 0x0018,
  value = 0x0019,
  reference_ticks = 0x001A,
  red = 0x0020,
  green = 0x0021,
  blue = 0x0022,
  audio_mode = 0x0023,
  low_frequency_hz = 0x0024,
  high_frequency_hz = 0x0025,
  amplitude = 0x0026,
  adc_value = 0x0027,
  rms_ns = 0x0030,
  skew_ppb = 0x0031,
  model_points = 0x0032,
  model_generation = 0x0033,
  interval_error_ppb = 0x0034,
  rssi_dbm = 0x0040,
  timeout_count = 0x0041,
  retry_count = 0x0042,
  connection_interval_us = 0x0043,
  gateway_receive_ticks = 0x0044,
  response_queued_ticks = 0x0045,
  transmit_start_ticks = 0x0046,
  transmit_complete_ticks = 0x0047,
  fragment_count = 0x0048,
  att_mtu = 0x0049,
  transport_decode_errors = 0x004A,
  transport_reassembly_errors = 0x004B,
  transport_transmit_errors = 0x004C,
  ttl_capture_count = 0x004E,
  ttl_capture_drops = 0x004F,
  detail = 0x0050,
  reason = 0x0051,
  test_mode = 0x0060,
  sensor_status = 0x0070,
  sample_rate_hz = 0x0071,
  sample_count = 0x0072,
  accel_x_mg = 0x0073,
  accel_y_mg = 0x0074,
  accel_z_mg = 0x0075,
  gyro_x_mdps = 0x0076,
  gyro_y_mdps = 0x0077,
  gyro_z_mdps = 0x0078,
  mag_x_milligauss = 0x0079,
  mag_y_milligauss = 0x007A,
  mag_z_milligauss = 0x007B,
  sensor_i2c_errors = 0x007C,
  chunk_sequence = 0x007D,
  packed_samples = 0x007E,
  sample_format = 0x007F,
  pmic_present = 0x0080,
  power_source = 0x0081,
  supply_millivolts = 0x0082,
  battery_millivolts = 0x0083,
  battery_current_ma = 0x0084,
  battery_soc_per_mille = 0x0085,
  battery_health_per_mille = 0x0086,
  raw_counts = 0x0087,
  sample_index = 0x0088,
  board_role = 0x0089,
  drive_mode = 0x008A,
  brightness = 0x008B,
  led_index = 0x008C,
  ready = 0x008D,
};

struct RecordHeader {
  RecordType type{RecordType::health};
  std::uint16_t flags{};
  std::uint32_t record_id{};
  std::uint32_t session_id{};
  std::uint64_t timestamp_ticks{};
  std::uint32_t clock_hz{};
  std::uint16_t payload_length{};
};

struct RecordView {
  RecordHeader header{};
  const std::uint8_t *payload{};
};

std::size_t encode_record(const RecordHeader &header,
                          const std::uint8_t *payload,
                          std::size_t payload_length, std::uint8_t *destination,
                          std::size_t capacity);

bool decode_record(const std::uint8_t *message, std::size_t length,
                   RecordView &output);

bool record_visible_at(RecordType type, TelemetryLevel level);

} // namespace fairy::protocol
