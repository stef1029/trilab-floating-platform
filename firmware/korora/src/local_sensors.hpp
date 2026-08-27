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
  bool vbus_present{};
  std::uint16_t battery_millivolts{};
  std::int32_t battery_current_ma{};
  bool battery_current_valid{};
  std::uint8_t charger_status{};
  std::uint8_t charger_error_reason{};
  std::uint8_t charger_error_sensor{};
  std::int32_t accel_mg[3]{};
  std::int32_t gyro_mdps[3]{};
  std::int32_t mag_milligauss[3]{};
};

// Starts the local I2C sensor worker. Missing sensors are reported in telemetry
// but do not prevent the rest of Korora from starting.
int initialize();

void set_telemetry(fairy::protocol::TelemetryLevel level);
void set_session(std::uint32_t session_id);

// Schematic D3 is nPM1300 LED0 and is reserved for Korora READY.
// Schematic D1/D2 are nPM1300 LED2/LED1 and are user-programmable.
bool set_ready_led(bool enabled);
bool set_status_led(std::uint8_t led_index, bool enabled);

Snapshot snapshot();

} // namespace korora_local_sensors
