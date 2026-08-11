#pragma once

#include <cstdint>

#include "fairy_shared/fairy_protocol.hpp"

namespace korora_local_sensors {

// Status bit mask used by Field::sensor_status.
inline constexpr std::uint8_t status_i2c_ready = 1U << 0U;
inline constexpr std::uint8_t status_imu_present = 1U << 1U;
inline constexpr std::uint8_t status_magnetometer_present = 1U << 2U;
inline constexpr std::uint8_t status_pmic_present = 1U << 3U;

struct Snapshot {
  std::uint8_t status{};
  std::uint32_t sample_count{};
  std::uint32_t i2c_errors{};
  std::int32_t accel_mg[3]{};
  std::int32_t gyro_mdps[3]{};
  std::int32_t mag_milligauss[3]{};
};

// Starts the local I2C sensor worker. Missing sensors are reported in telemetry
// but do not prevent the rest of Korora from starting.
int initialize();

void set_telemetry(fairy::protocol::TelemetryLevel level);
void set_session(std::uint32_t session_id);
Snapshot snapshot();

} // namespace korora_local_sensors
