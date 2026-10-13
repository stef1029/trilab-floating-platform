#include "local_sensors.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "ble_gateway.hpp"
#include "debug_log.hpp"
#include "fairy_shared/bytes.hpp"
#include "fairy_shared/fairy_protocol.hpp"
#include "fairy_shared/system_config.hpp"
#include "fairy_shared/tlv.hpp"
#include "timebase.hpp"

namespace korora_local_sensors {
namespace {

// LSM6DSV32X, SDO/SA0 tied low on the prototype.
inline constexpr std::uint16_t imu_address = 0x6AU;
inline constexpr std::uint8_t imu_who_am_i = 0x0FU;
inline constexpr std::uint8_t imu_who_am_i_value = 0x70U;
inline constexpr std::uint8_t imu_ctrl1 = 0x10U;
inline constexpr std::uint8_t imu_ctrl2 = 0x11U;
inline constexpr std::uint8_t imu_ctrl3 = 0x12U;
inline constexpr std::uint8_t imu_ctrl6 = 0x15U;
inline constexpr std::uint8_t imu_outx_l_g = 0x22U;

// MMC5983MA fixed I2C address.
inline constexpr std::uint16_t mag_address = 0x30U;
inline constexpr std::uint8_t mag_status = 0x08U;
inline constexpr std::uint8_t mag_control0 = 0x09U;
inline constexpr std::uint8_t mag_control1 = 0x0AU;
inline constexpr std::uint8_t mag_control2 = 0x0BU;
inline constexpr std::uint8_t mag_product_id = 0x2FU;
inline constexpr std::uint8_t mag_product_id_value = 0x30U;
inline constexpr std::uint8_t mag_xout0 = 0x00U;

// nPM1300 uses a 7-bit I2C address and 16-bit register addresses.
inline constexpr std::uint16_t pmic_address = 0x6BU;
inline constexpr std::uint16_t pmic_vbus_update_ilim = 0x0200U;
inline constexpr std::uint16_t pmic_vbus_ilim = 0x0201U;
inline constexpr std::uint16_t pmic_charge_enable_set = 0x0304U;
inline constexpr std::uint16_t pmic_charge_enable_clear = 0x0305U;
inline constexpr std::uint16_t pmic_charge_disable_clear = 0x0307U;
inline constexpr std::uint16_t pmic_charge_current_msb = 0x0308U;
inline constexpr std::uint16_t pmic_charge_current_lsb = 0x0309U;
inline constexpr std::uint16_t configured_charge_current_ma = 200U;
inline constexpr std::uint16_t pmic_charge_vterm = 0x030CU;
inline constexpr std::uint16_t pmic_charge_vterm_reduced = 0x030DU;
inline constexpr std::uint8_t configured_vterm = 11U; // 4.35 V LiHV
inline constexpr std::uint16_t pmic_vbus_status = 0x0207U;
inline constexpr std::uint16_t pmic_task_vbat_measure = 0x0500U;
inline constexpr std::uint16_t pmic_ibat_status = 0x0510U;
inline constexpr std::uint16_t pmic_vbat_result_msb = 0x0511U;
inline constexpr std::uint16_t pmic_gp0_result_lsbs = 0x0515U;
inline constexpr std::uint16_t pmic_ibat_result_msb = 0x0518U;
inline constexpr std::uint16_t pmic_gp1_result_lsbs = 0x051AU;
inline constexpr std::uint16_t pmic_ibat_measure_enable = 0x0524U;
inline constexpr std::uint16_t pmic_discharge_limit_msb = 0x030AU;
inline constexpr std::uint16_t pmic_discharge_limit_lsb = 0x030BU;
inline constexpr std::uint16_t pmic_charge_status = 0x0334U;
inline constexpr std::uint16_t pmic_charge_error_reason = 0x0336U;
inline constexpr std::uint16_t pmic_charge_error_sensor = 0x0337U;
inline constexpr std::uint16_t pmic_led_mode_base = 0x0A00U;
inline constexpr std::uint16_t pmic_led_set_base = 0x0A03U;
inline constexpr std::uint16_t pmic_led_clear_base = 0x0A04U;
inline constexpr std::uint8_t pmic_led_mode_host = 2U;

// LEDDRV has fixed ~5 mA current when enabled; there is no hardware PWM or
// brightness register. Keep the indicators static rather than generating
// continuous I2C traffic to fake dimming.
std::uint16_t pmic_charge_current_ma{};
std::uint16_t pmic_discharge_limit_ma{};

inline constexpr std::uint16_t sample_rate_hz = 120U;
inline constexpr std::uint32_t sample_period_us = 1'000'000U / sample_rate_hz;
inline constexpr std::uint32_t live_period_ms = 100U;
inline constexpr std::uint32_t power_period_ms = 1000U;

// Packed IMU dump format v1:
//   uint32 delta_ticks
//   int16  accel_raw[3]
//   int16  gyro_raw[3]
//   int32  mag_raw_centered[3]
// = 28 bytes per sample. Seven samples plus TLV metadata fit under the
// existing 256-byte Fairy record payload maximum.
inline constexpr std::uint8_t sample_format_v1 = 1U;
inline constexpr std::size_t packed_sample_size = 28U;
inline constexpr std::size_t samples_per_chunk = 7U;

const device *const i2c_bus = DEVICE_DT_GET(DT_NODELABEL(i2c1));

atomic_t session_id;
atomic_t telemetry_level = ATOMIC_INIT(1);
atomic_t record_id;
atomic_t chunk_sequence;
atomic_t flush_requested;

k_mutex state_mutex;
k_mutex i2c_mutex;
Snapshot current_state;

K_THREAD_STACK_DEFINE(sensor_stack, 4096);
k_thread sensor_thread_data;
k_tid_t sensor_thread_id;

struct RawSample {
  std::uint64_t ticks{};
  std::int16_t accel[3]{};
  std::int16_t gyro[3]{};
  std::int32_t mag[3]{};
};

std::array<RawSample, samples_per_chunk> chunk{};
std::size_t chunk_count{};
std::uint32_t chunk_session{};

std::int16_t get_i16(const std::uint8_t *data) {
  return static_cast<std::int16_t>(fairy::wire::get_u16(data));
}

void put_i16(std::uint8_t *destination, std::int16_t value) {
  fairy::wire::put_u16(destination, static_cast<std::uint16_t>(value));
}

void put_i32(std::uint8_t *destination, std::int32_t value) {
  fairy::wire::put_u32(destination, static_cast<std::uint32_t>(value));
}

std::uint32_t next_record_id() {
  return static_cast<std::uint32_t>(atomic_inc(&record_id)) + 1U;
}

int reg_read_byte(std::uint16_t address, std::uint8_t reg,
                  std::uint8_t *value) {
  k_mutex_lock(&i2c_mutex, K_FOREVER);
  const int result = i2c_reg_read_byte(i2c_bus, address, reg, value);
  k_mutex_unlock(&i2c_mutex);
  return result;
}

int reg_write_byte(std::uint16_t address, std::uint8_t reg,
                   std::uint8_t value) {
  k_mutex_lock(&i2c_mutex, K_FOREVER);
  const int result = i2c_reg_write_byte(i2c_bus, address, reg, value);
  k_mutex_unlock(&i2c_mutex);
  return result;
}

int burst_read(std::uint16_t address, std::uint8_t reg, std::uint8_t *value,
               std::size_t length) {
  k_mutex_lock(&i2c_mutex, K_FOREVER);
  const int result = i2c_burst_read(i2c_bus, address, reg, value, length);
  k_mutex_unlock(&i2c_mutex);
  return result;
}

int pmic_read(std::uint16_t reg, std::uint8_t &value) {
  const std::uint8_t address[2] = {
      static_cast<std::uint8_t>(reg >> 8U),
      static_cast<std::uint8_t>(reg & 0xFFU),
  };
  k_mutex_lock(&i2c_mutex, K_FOREVER);
  const int result = i2c_write_read(i2c_bus, pmic_address, address,
                                    sizeof(address), &value, sizeof(value));
  k_mutex_unlock(&i2c_mutex);
  return result;
}

int pmic_write(std::uint16_t reg, std::uint8_t value) {
  const std::uint8_t data[3] = {
      static_cast<std::uint8_t>(reg >> 8U),
      static_cast<std::uint8_t>(reg & 0xFFU),
      value,
  };
  k_mutex_lock(&i2c_mutex, K_FOREVER);
  const int result = i2c_write(i2c_bus, data, sizeof(data), pmic_address);
  k_mutex_unlock(&i2c_mutex);
  return result;
}

std::uint8_t status_bits() {
  k_mutex_lock(&state_mutex, K_FOREVER);
  const std::uint8_t value = current_state.status;
  k_mutex_unlock(&state_mutex);
  return value;
}

void set_status_bit(std::uint8_t mask, bool enabled) {
  k_mutex_lock(&state_mutex, K_FOREVER);
  if (enabled) {
    current_state.status |= mask;
  } else {
    current_state.status &= static_cast<std::uint8_t>(~mask);
  }
  k_mutex_unlock(&state_mutex);
}

void note_i2c_error() {
  k_mutex_lock(&state_mutex, K_FOREVER);
  ++current_state.i2c_errors;
  k_mutex_unlock(&state_mutex);
}

bool publish_record(fairy::protocol::RecordType type, std::uint32_t session,
                    std::uint64_t timestamp_ticks, const std::uint8_t *payload,
                    std::size_t payload_length,
                    bool diagnostic_priority = false) {
  fairy::protocol::RecordHeader header;
  header.type = type;
  header.record_id = next_record_id();
  header.session_id = session;
  header.timestamp_ticks = timestamp_ticks;
  header.clock_hz = fairy::config::common_timer_hz;

  std::uint8_t record[fairy::protocol::fairy_max_record_size]{};
  const std::size_t length = fairy::protocol::encode_record(
      header, payload, payload_length, record, sizeof(record));
  if (length == 0U) {
    return false;
  }

  return korora_ble::send_to_adelie(fairy::config::korora_address,
                                    fairy::transport::Channel::fairy, record,
                                    length, 0U, 0U, 0U, diagnostic_priority);
}

bool probe_imu() {
  std::uint8_t who{};
  if (reg_read_byte(imu_address, imu_who_am_i, &who) != 0 ||
      who != imu_who_am_i_value) {
    note_i2c_error();
    return false;
  }

  // CTRL3 defaults to BDU=1 and IF_INC=1. Reassert the documented value so
  // multi-byte reads remain coherent after any previous prototype firmware.
  if (reg_write_byte(imu_address, imu_ctrl3, 0x44U) != 0 ||
      // ±500 dps; default LPF selection.
      reg_write_byte(imu_address, imu_ctrl6, 0x02U) != 0 ||
      // High-performance 120 Hz accelerometer and gyro.
      reg_write_byte(imu_address, imu_ctrl1, 0x06U) != 0 ||
      reg_write_byte(imu_address, imu_ctrl2, 0x06U) != 0) {
    note_i2c_error();
    return false;
  }

  return true;
}

inline constexpr std::uint16_t pmic_usb_c_detect_status = 0x0205U;

bool configure_vbus_current_limit() {
  std::uint8_t cc{};
  if (pmic_read(pmic_usb_c_detect_status, cc) != 0) {
    return false;
  }

  const std::uint8_t cc1 = cc & 0x03U;
  const std::uint8_t cc2 = (cc >> 2U) & 0x03U;
  const std::uint8_t source = cc1 > cc2 ? cc1 : cc2;

  // nPM1300 VBUSINILIM0 encoding.
  std::uint8_t limit = 1U; // 100 mA fallback

  switch (source) {
  case 3U: // USB-C source advertises 3 A
  case 2U: // USB-C source advertises 1.5 A
    // nPM1300 itself tops out at 1.5 A.
    limit = 15U;
    break;

  case 1U:      // Default USB source
    limit = 5U; // 500 mA
    break;

  default:
    limit = 1U; // 100 mA
    break;
  }

  return pmic_write(pmic_vbus_ilim, limit) == 0 &&
         pmic_write(pmic_vbus_update_ilim, 1U) == 0;
}

bool configure_charger() {
  // Charger must be disabled before changing ICHG.
  if (pmic_write(pmic_charge_enable_clear, 1U) != 0) {
    return false;
  }

  // ICHG encoding is a 9-bit value in 2 mA steps.
  const std::uint16_t charge_code = configured_charge_current_ma / 2U;

  const std::uint8_t charge_msb = static_cast<std::uint8_t>(charge_code >> 1U);

  const std::uint8_t charge_lsb =
      static_cast<std::uint8_t>(charge_code & 0x01U);

  if (pmic_write(pmic_charge_current_msb, charge_msb) != 0 ||
      pmic_write(pmic_charge_current_lsb, charge_lsb) != 0) {
    return false;
  }

  // Normal/cool and warm-region termination voltage.
  if (pmic_write(pmic_charge_vterm, configured_vterm) != 0 ||
      pmic_write(pmic_charge_vterm_reduced, configured_vterm) != 0) {
    return false;
  }

  // Make sure automatic recharge is enabled.
  // BCHGDISABLECLR bit 0 = clear DISABLERECHARGE.
  if (pmic_write(pmic_charge_disable_clear, 0x01U) != 0) {
    return false;
  }

  // Enable battery charging.
  //
  // Bit 0 = ENABLECHARGING.
  // We intentionally leave bit 1 clear, so the nPM1300 retains
  // its conservative cool-temperature current behaviour.
  if (pmic_write(pmic_charge_enable_set, 0x01U) != 0) {
    return false;
  }

  return true;
}

bool probe_pmic() {
  std::uint8_t status{};
  if (pmic_read(pmic_vbus_status, status) != 0) {
    note_i2c_error();
    return false;
  }

  // Enable an IBAT conversion after each requested VBAT conversion.
  if (pmic_write(pmic_ibat_measure_enable, 1U) != 0) {
    note_i2c_error();
    return false;
  }

  if (!configure_vbus_current_limit()) {
    note_i2c_error();
    return false;
  }

  if (!configure_charger()) {
    note_i2c_error();
    return false;
  }

  // Cache the configured charge/discharge ranges used to scale IBAT.
  std::uint8_t charge_msb{};
  std::uint8_t charge_lsb{};
  std::uint8_t discharge_msb{};
  std::uint8_t discharge_lsb{};
  if (pmic_read(pmic_charge_current_msb, charge_msb) != 0 ||
      pmic_read(pmic_charge_current_lsb, charge_lsb) != 0 ||
      pmic_read(pmic_discharge_limit_msb, discharge_msb) != 0 ||
      pmic_read(pmic_discharge_limit_lsb, discharge_lsb) != 0) {
    note_i2c_error();
    return false;
  }
  const std::uint16_t charge_code =
      (static_cast<std::uint16_t>(charge_msb) << 1U) | (charge_lsb & 0x01U);
  pmic_charge_current_ma = static_cast<std::uint16_t>(charge_code * 2U);

  const std::uint16_t discharge_code =
      (static_cast<std::uint16_t>(discharge_msb) << 1U) |
      (discharge_lsb & 0x01U);
  // nPM1300 exposes two supported discharge ranges: 200 mA and 1 A.
  pmic_discharge_limit_ma = discharge_code == 84U    ? 200U
                            : discharge_code == 415U ? 1000U
                                                     : 0U;

  // Put all three nPM1300 LED sinks into host mode. In this schematic:
  // D1 -> LED2, D2 -> LED1, D3 -> LED0. Start all three dark.
  for (std::uint8_t index = 0; index < 3U; ++index) {
    if (pmic_write(pmic_led_mode_base + index, pmic_led_mode_host) != 0 ||
        pmic_write(pmic_led_clear_base + 2U * index, 1U) != 0) {
      note_i2c_error();
      return false;
    }
  }
  return true;
}

const char *charge_status_text(std::uint8_t status, bool vbus_present) {
  if ((status & (1U << 6U)) != 0U) {
    return "Charging paused - PMIC hot";
  }
  if ((status & (1U << 1U)) != 0U) {
    return "Full";
  }
  if ((status & (1U << 2U)) != 0U) {
    return "Charging - trickle";
  }
  if ((status & (1U << 3U)) != 0U) {
    return "Charging - constant current";
  }
  if ((status & (1U << 4U)) != 0U) {
    return "Charging - constant voltage";
  }
  if ((status & (1U << 7U)) != 0U) {
    return "Supplement mode";
  }
  if ((status & (1U << 5U)) != 0U) {
    return "Recharge requested";
  }
  return vbus_present ? "USB - charger idle" : "Discharging";
}

const char *charge_error_text(std::uint8_t reason) {
  if ((reason & (1U << 0U)) != 0U) {
    return "NTC sensor error";
  }
  if ((reason & (1U << 1U)) != 0U) {
    return "VBAT sensor error";
  }
  if ((reason & (1U << 2U)) != 0U) {
    return "Battery-low charger error";
  }
  if ((reason & (1U << 3U)) != 0U) {
    return "Trickle-voltage charger error";
  }
  if ((reason & (1U << 4U)) != 0U) {
    return "Measurement timeout";
  }
  if ((reason & (1U << 5U)) != 0U) {
    return "Charge timeout";
  }
  if ((reason & (1U << 6U)) != 0U) {
    return "Trickle-charge timeout";
  }
  return "";
}

bool read_pmic_power() {
  if ((status_bits() & status_pmic_present) == 0U) {
    return false;
  }

  if (pmic_write(pmic_task_vbat_measure, 1U) != 0) {
    note_i2c_error();
    return false;
  }

  // VBAT + enabled IBAT are consecutive 250 us conversions.
  k_sleep(K_MSEC(1));

  std::uint8_t vbat_msb{};
  std::uint8_t gp0_lsbs{};
  std::uint8_t ibat_status{};
  std::uint8_t ibat_msb{};
  std::uint8_t gp1_lsbs{};
  std::uint8_t vbus_status{};
  std::uint8_t charger_status{};
  std::uint8_t charger_error_reason{};
  std::uint8_t charger_error_sensor{};
  if (pmic_read(pmic_vbat_result_msb, vbat_msb) != 0 ||
      pmic_read(pmic_gp0_result_lsbs, gp0_lsbs) != 0 ||
      pmic_read(pmic_ibat_status, ibat_status) != 0 ||
      pmic_read(pmic_ibat_result_msb, ibat_msb) != 0 ||
      pmic_read(pmic_gp1_result_lsbs, gp1_lsbs) != 0 ||
      pmic_read(pmic_vbus_status, vbus_status) != 0 ||
      pmic_read(pmic_charge_status, charger_status) != 0 ||
      pmic_read(pmic_charge_error_reason, charger_error_reason) != 0 ||
      pmic_read(pmic_charge_error_sensor, charger_error_sensor) != 0) {
    note_i2c_error();
    return false;
  }

  const std::uint16_t vbat_raw =
      (static_cast<std::uint16_t>(vbat_msb) << 2U) | (gp0_lsbs & 0x03U);
  const std::uint16_t millivolts = static_cast<std::uint16_t>(
      (static_cast<std::uint32_t>(vbat_raw) * 5000U + 512U) / 1024U);

  bool current_valid = (ibat_status & 0x10U) == 0U;
  std::int32_t current_ma = 0;
  if (current_valid) {
    const std::uint16_t ibat_raw =
        (static_cast<std::uint16_t>(ibat_msb) << 2U) |
        ((gp1_lsbs >> 4U) & 0x03U);
    const std::uint8_t mode = ibat_status & 0x0CU;
    std::int32_t full_scale_ma = 0;
    if (mode == 0x04U && pmic_discharge_limit_ma != 0U) {
      full_scale_ma = -static_cast<std::int32_t>(
          (static_cast<std::uint32_t>(pmic_discharge_limit_ma) * 112U) / 100U);
    } else if (mode == 0x0CU && pmic_charge_current_ma != 0U) {
      full_scale_ma = static_cast<std::int32_t>(
          (static_cast<std::uint32_t>(pmic_charge_current_ma) * 125U) / 100U);
    } else if (mode != 0x08U) {
      current_valid = false;
    }
    if (current_valid && full_scale_ma != 0) {
      current_ma = static_cast<std::int32_t>(
          (static_cast<std::int64_t>(ibat_raw) * full_scale_ma) / 1023LL);
    }
  }

  const bool vbus_present = (vbus_status & 0x01U) != 0U;

  bool was_vbus_present{};
  k_mutex_lock(&state_mutex, K_FOREVER);
  was_vbus_present = current_state.vbus_present;
  k_mutex_unlock(&state_mutex);

  // VBUS has just been connected.
  //
  // Allow the nPM1300 CC detector to settle and then switch from
  // VBUSINILIMSTARTUP to the source-appropriate software limit.
  if (vbus_present && !was_vbus_present) {
    k_sleep(K_MSEC(20));

    if (!configure_vbus_current_limit()) {
      note_i2c_error();
    }
  }

  k_mutex_lock(&state_mutex, K_FOREVER);
  current_state.battery_millivolts = millivolts;
  current_state.battery_current_ma = current_ma;
  current_state.battery_current_valid = current_valid;
  current_state.vbus_present = vbus_present;
  current_state.charger_status = charger_status;
  current_state.charger_error_reason = charger_error_reason;
  current_state.charger_error_sensor = charger_error_sensor;
  k_mutex_unlock(&state_mutex);

  return true;
}

bool probe_magnetometer() {
  std::uint8_t product{};
  if (reg_read_byte(mag_address, mag_product_id, &product) != 0 ||
      product != mag_product_id_value) {
    note_i2c_error();
    return false;
  }

  // BW=00 gives the 18-bit, 8 ms measurement path. Auto set/reset is useful
  // for a prototype around changing magnetic environments. Continuous mode at
  // 100 Hz is Cmm_en=1, CM_Freq=101.
  if (reg_write_byte(mag_address, mag_control0, 0x20U) != 0 ||
      reg_write_byte(mag_address, mag_control1, 0x00U) != 0 ||
      reg_write_byte(mag_address, mag_control2, 0x0DU) != 0) {
    note_i2c_error();
    return false;
  }

  return true;
}

bool read_imu(RawSample &sample) {
  std::uint8_t raw[12]{};
  if (burst_read(imu_address, imu_outx_l_g, raw, sizeof(raw)) != 0) {
    note_i2c_error();
    return false;
  }

  sample.gyro[0] = get_i16(raw + 0);
  sample.gyro[1] = get_i16(raw + 2);
  sample.gyro[2] = get_i16(raw + 4);
  sample.accel[0] = get_i16(raw + 6);
  sample.accel[1] = get_i16(raw + 8);
  sample.accel[2] = get_i16(raw + 10);
  return true;
}

bool update_magnetometer(RawSample &sample) {
  std::uint8_t ready{};
  if (reg_read_byte(mag_address, mag_status, &ready) != 0) {
    note_i2c_error();
    return false;
  }

  if ((ready & 0x01U) == 0U) {
    return true;
  }

  std::uint8_t raw[7]{};
  if (burst_read(mag_address, mag_xout0, raw, sizeof(raw)) != 0) {
    note_i2c_error();
    return false;
  }

  const std::uint32_t x = (static_cast<std::uint32_t>(raw[0]) << 10U) |
                          (static_cast<std::uint32_t>(raw[1]) << 2U) |
                          ((raw[6] >> 6U) & 0x03U);
  const std::uint32_t y = (static_cast<std::uint32_t>(raw[2]) << 10U) |
                          (static_cast<std::uint32_t>(raw[3]) << 2U) |
                          ((raw[6] >> 4U) & 0x03U);
  const std::uint32_t z = (static_cast<std::uint32_t>(raw[4]) << 10U) |
                          (static_cast<std::uint32_t>(raw[5]) << 2U) |
                          ((raw[6] >> 2U) & 0x03U);

  sample.mag[0] = static_cast<std::int32_t>(x) - 131072;
  sample.mag[1] = static_cast<std::int32_t>(y) - 131072;
  sample.mag[2] = static_cast<std::int32_t>(z) - 131072;
  return true;
}

std::int32_t accel_to_mg(std::int16_t raw) {
  // LSM6DSV32X ±4 g sensitivity: 0.122 mg/LSB.
  return static_cast<std::int32_t>(raw) * 122 / 1000;
}

std::int32_t gyro_to_mdps(std::int16_t raw) {
  // LSM6DSV32X ±500 dps sensitivity: 17.5 mdps/LSB.
  return static_cast<std::int32_t>(raw) * 175 / 10;
}

std::int32_t mag_to_milligauss(std::int32_t raw) {
  // MMC5983MA 18-bit sensitivity: 16384 counts/G.
  return static_cast<std::int32_t>((static_cast<std::int64_t>(raw) * 1000LL) /
                                   16384LL);
}

void update_snapshot(const RawSample &sample) {
  k_mutex_lock(&state_mutex, K_FOREVER);
  ++current_state.sample_count;
  for (std::size_t axis = 0; axis < 3U; ++axis) {
    current_state.accel_mg[axis] = accel_to_mg(sample.accel[axis]);
    current_state.gyro_mdps[axis] = gyro_to_mdps(sample.gyro[axis]);
    current_state.mag_milligauss[axis] = mag_to_milligauss(sample.mag[axis]);
  }
  k_mutex_unlock(&state_mutex);
}

void publish_live(std::uint64_t timestamp_ticks) {
  if (static_cast<int>(atomic_get(&telemetry_level)) <
      static_cast<int>(fairy::protocol::TelemetryLevel::standard)) {
    return;
  }

  const Snapshot state = snapshot();
  std::uint8_t payload[fairy::protocol::fairy_max_payload]{};
  fairy::protocol::TlvWriter fields(payload, sizeof(payload));
  fields.u8(static_cast<std::uint16_t>(fairy::protocol::Field::sensor_status),
            state.status);
  fields.u16(static_cast<std::uint16_t>(fairy::protocol::Field::sample_rate_hz),
             sample_rate_hz);
  fields.u32(static_cast<std::uint16_t>(fairy::protocol::Field::sample_count),
             state.sample_count);
  fields.u32(
      static_cast<std::uint16_t>(fairy::protocol::Field::sensor_i2c_errors),
      state.i2c_errors);
  fields.i32(static_cast<std::uint16_t>(fairy::protocol::Field::accel_x_mg),
             state.accel_mg[0]);
  fields.i32(static_cast<std::uint16_t>(fairy::protocol::Field::accel_y_mg),
             state.accel_mg[1]);
  fields.i32(static_cast<std::uint16_t>(fairy::protocol::Field::accel_z_mg),
             state.accel_mg[2]);
  fields.i32(static_cast<std::uint16_t>(fairy::protocol::Field::gyro_x_mdps),
             state.gyro_mdps[0]);
  fields.i32(static_cast<std::uint16_t>(fairy::protocol::Field::gyro_y_mdps),
             state.gyro_mdps[1]);
  fields.i32(static_cast<std::uint16_t>(fairy::protocol::Field::gyro_z_mdps),
             state.gyro_mdps[2]);
  fields.i32(
      static_cast<std::uint16_t>(fairy::protocol::Field::mag_x_milligauss),
      state.mag_milligauss[0]);
  fields.i32(
      static_cast<std::uint16_t>(fairy::protocol::Field::mag_y_milligauss),
      state.mag_milligauss[1]);
  fields.i32(
      static_cast<std::uint16_t>(fairy::protocol::Field::mag_z_milligauss),
      state.mag_milligauss[2]);

  if (fields.good()) {
    (void)publish_record(fairy::protocol::RecordType::local_sensors,
                         static_cast<std::uint32_t>(atomic_get(&session_id)),
                         timestamp_ticks, payload, fields.size(), false);
  }
}

void publish_power(std::uint64_t timestamp_ticks) {
  if (static_cast<int>(atomic_get(&telemetry_level)) <
      static_cast<int>(fairy::protocol::TelemetryLevel::standard)) {
    return;
  }

  const bool pmic_present = (status_bits() & status_pmic_present) != 0U;
  if (pmic_present) {
    (void)read_pmic_power();
  }
  const Snapshot state = snapshot();

  std::uint8_t payload[fairy::protocol::fairy_max_payload]{};
  fairy::protocol::TlvWriter fields(payload, sizeof(payload));
  fields.boolean(
      static_cast<std::uint16_t>(fairy::protocol::Field::pmic_present),
      pmic_present);
  fields.string(
      static_cast<std::uint16_t>(fairy::protocol::Field::power_source),
      pmic_present ? (state.vbus_present ? "nPM1300 VBUS" : "nPM1300 battery")
                   : "nPM1300 unavailable");
  if (pmic_present && state.battery_millivolts != 0U) {
    fields.u16(
        static_cast<std::uint16_t>(fairy::protocol::Field::battery_millivolts),
        state.battery_millivolts);
  }
  if (pmic_present && state.battery_current_valid) {
    fields.i32(
        static_cast<std::uint16_t>(fairy::protocol::Field::battery_current_ma),
        state.battery_current_ma);
  }
  if (pmic_present) {
    fields.string(static_cast<std::uint16_t>(fairy::protocol::Field::detail),
                  charge_status_text(state.charger_status, state.vbus_present));
    if (state.charger_error_reason != 0U) {
      fields.string(static_cast<std::uint16_t>(fairy::protocol::Field::reason),
                    charge_error_text(state.charger_error_reason));
    }
  }

  if (fields.good()) {
    (void)publish_record(fairy::protocol::RecordType::power_status,
                         static_cast<std::uint32_t>(atomic_get(&session_id)),
                         timestamp_ticks, payload, fields.size(), false);
  }
}

void publish_chunk() {
  if (chunk_count == 0U) {
    return;
  }

  std::uint8_t packed[samples_per_chunk * packed_sample_size]{};
  const std::uint64_t first_ticks = chunk[0].ticks;
  for (std::size_t index = 0; index < chunk_count; ++index) {
    std::uint8_t *const destination = packed + index * packed_sample_size;
    const std::uint64_t delta64 = chunk[index].ticks - first_ticks;
    const std::uint32_t delta = delta64 > 0xFFFF'FFFFULL
                                    ? 0xFFFF'FFFFU
                                    : static_cast<std::uint32_t>(delta64);
    fairy::wire::put_u32(destination, delta);
    put_i16(destination + 4, chunk[index].accel[0]);
    put_i16(destination + 6, chunk[index].accel[1]);
    put_i16(destination + 8, chunk[index].accel[2]);
    put_i16(destination + 10, chunk[index].gyro[0]);
    put_i16(destination + 12, chunk[index].gyro[1]);
    put_i16(destination + 14, chunk[index].gyro[2]);
    put_i32(destination + 16, chunk[index].mag[0]);
    put_i32(destination + 20, chunk[index].mag[1]);
    put_i32(destination + 24, chunk[index].mag[2]);
  }

  std::uint8_t payload[fairy::protocol::fairy_max_payload]{};
  fairy::protocol::TlvWriter fields(payload, sizeof(payload));
  fields.u8(static_cast<std::uint16_t>(fairy::protocol::Field::sensor_status),
            status_bits());
  fields.u8(static_cast<std::uint16_t>(fairy::protocol::Field::sample_format),
            sample_format_v1);
  fields.u16(static_cast<std::uint16_t>(fairy::protocol::Field::sample_rate_hz),
             sample_rate_hz);
  fields.u32(static_cast<std::uint16_t>(fairy::protocol::Field::sample_count),
             static_cast<std::uint32_t>(chunk_count));
  fields.u32(static_cast<std::uint16_t>(fairy::protocol::Field::chunk_sequence),
             static_cast<std::uint32_t>(atomic_inc(&chunk_sequence)) + 1U);
  fields.bytes(
      static_cast<std::uint16_t>(fairy::protocol::Field::packed_samples),
      packed, static_cast<std::uint8_t>(chunk_count * packed_sample_size));

  if (fields.good() &&
      !publish_record(fairy::protocol::RecordType::imu_samples, chunk_session,
                      first_ticks, payload, fields.size(), false)) {
    korora_debug::log("LOCAL_SENSOR imu_chunk_drop count=%u\r\n",
                      static_cast<unsigned>(chunk_count));
  }

  chunk_count = 0U;
  chunk_session = 0U;
}

void append_chunk(const RawSample &sample, std::uint32_t session) {
  if (chunk_count == 0U) {
    chunk_session = session;
  }

  if (session != chunk_session) {
    publish_chunk();
    chunk_session = session;
  }

  chunk[chunk_count++] = sample;
  if (chunk_count >= samples_per_chunk) {
    publish_chunk();
  }
}

void sensor_worker(void *, void *, void *) {
  const bool bus_ready = device_is_ready(i2c_bus);
  set_status_bit(status_i2c_ready, bus_ready);

  if (!bus_ready) {
    korora_debug::log("LOCAL_SENSOR i2c1_not_ready\r\n");
  }

  bool imu_present = false;
  bool mag_present = false;
  if (bus_ready) {
    imu_present = probe_imu();
    mag_present = probe_magnetometer();
  }
  set_status_bit(status_imu_present, imu_present);
  set_status_bit(status_magnetometer_present, mag_present);

  korora_debug::log("LOCAL_SENSOR i2c=%u imu=%u mag=%u pmic=%u\r\n",
                    bus_ready ? 1U : 0U, imu_present ? 1U : 0U,
                    mag_present ? 1U : 0U,
                    (status_bits() & status_pmic_present) != 0U ? 1U : 0U);

  RawSample latest{};
  std::uint64_t next_live_ticks = 0U;
  std::uint64_t next_power_ticks = 0U;
  const std::uint64_t live_period_ticks =
      fairy::config::common_timer_hz * live_period_ms / 1000U;
  const std::uint64_t power_period_ticks =
      fairy::config::common_timer_hz * power_period_ms / 1000U;

  // Allow the IMU's gyro path to settle before the first sample is published.
  k_sleep(K_MSEC(60));

  while (true) {
    if (atomic_cas(&flush_requested, 1, 0)) {
      publish_chunk();
    }

    const std::uint64_t now = korora_time::now();
    latest.ticks = now;

    if (imu_present && read_imu(latest)) {
      if (mag_present) {
        (void)update_magnetometer(latest);
      }
      update_snapshot(latest);

      const std::uint32_t session =
          static_cast<std::uint32_t>(atomic_get(&session_id));
      const bool full_telemetry =
          static_cast<int>(atomic_get(&telemetry_level)) >=
          static_cast<int>(fairy::protocol::TelemetryLevel::full);
      if (session != 0U || full_telemetry) {
        append_chunk(latest, session);
      } else if (chunk_count != 0U) {
        publish_chunk();
      }
    }

    if (next_live_ticks == 0U || now >= next_live_ticks) {
      publish_live(now);
      next_live_ticks = now + live_period_ticks;
    }
    if (next_power_ticks == 0U || now >= next_power_ticks) {
      publish_power(now);
      next_power_ticks = now + power_period_ticks;
    }

    k_sleep(K_USEC(sample_period_us));
  }
}

} // namespace

int initialize() {
  k_mutex_init(&state_mutex);
  k_mutex_init(&i2c_mutex);
  atomic_clear(&session_id);
  atomic_clear(&record_id);
  atomic_clear(&chunk_sequence);
  atomic_clear(&flush_requested);
  current_state = {};
  chunk_count = 0U;

  const bool bus_ready = device_is_ready(i2c_bus);
  set_status_bit(status_i2c_ready, bus_ready);
  const bool pmic_present = bus_ready && probe_pmic();
  set_status_bit(status_pmic_present, pmic_present);

  sensor_thread_id = k_thread_create(
      &sensor_thread_data, sensor_stack, K_THREAD_STACK_SIZEOF(sensor_stack),
      sensor_worker, nullptr, nullptr, nullptr, 7, 0, K_NO_WAIT);
  return sensor_thread_id == nullptr ? -1 : 0;
}

void set_telemetry(fairy::protocol::TelemetryLevel level) {
  atomic_set(&telemetry_level, static_cast<atomic_val_t>(level));
}

void set_session(std::uint32_t new_session) {
  const std::uint32_t previous =
      static_cast<std::uint32_t>(atomic_get(&session_id));
  atomic_set(&session_id, static_cast<atomic_val_t>(new_session));
  if (previous != 0U && new_session == 0U) {
    atomic_set(&flush_requested, 1);
  }
}

bool set_status_led(std::uint8_t led_index, bool enabled) {
  if (led_index > 2U || (status_bits() & status_pmic_present) == 0U) {
    return false;
  }

  const std::uint16_t reg =
      (enabled ? pmic_led_set_base : pmic_led_clear_base) + 2U * led_index;
  if (pmic_write(reg, 1U) != 0) {
    note_i2c_error();
    return false;
  }
  return true;
}

bool set_ready_led(bool enabled) { return set_status_led(0U, enabled); }

Snapshot snapshot() {
  k_mutex_lock(&state_mutex, K_FOREVER);
  const Snapshot result = current_state;
  k_mutex_unlock(&state_mutex);
  return result;
}

} // namespace korora_local_sensors
