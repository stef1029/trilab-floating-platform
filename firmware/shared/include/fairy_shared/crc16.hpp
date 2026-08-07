#pragma once

#include <cstddef>
#include <cstdint>

namespace fairy::wire {

inline std::uint16_t crc16_ccitt(const std::uint8_t *data, std::size_t length) {
  std::uint16_t crc = 0xFFFFU;
  for (std::size_t i = 0; i < length; ++i) {
    crc ^= static_cast<std::uint16_t>(data[i]) << 8U;
    for (unsigned int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000U) != 0U
                ? static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U)
                : static_cast<std::uint16_t>(crc << 1U);
    }
  }
  return crc;
}

} // namespace fairy::wire
