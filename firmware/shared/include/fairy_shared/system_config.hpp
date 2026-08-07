#pragma once

#include <cstddef>
#include <cstdint>

/*
 * Change this one value when building a rig with a different maximum number
 * of Fairy boards. The prototype can be built with -DFAIRY_MAX_BOARDS=2.
 */
#ifndef FAIRY_MAX_BOARDS
#define FAIRY_MAX_BOARDS 2
#endif

#ifndef FAIRY_ENABLE_DEBUG_STREAM
#define FAIRY_ENABLE_DEBUG_STREAM 1
#endif

namespace fairy::config {

inline constexpr std::size_t max_fairies = FAIRY_MAX_BOARDS;
static_assert(max_fairies >= 1 && max_fairies <= 14,
              "FAIRY_MAX_BOARDS must be between 1 and 14");

inline constexpr std::uint8_t unassigned_address = 0x00;
inline constexpr std::uint8_t korora_address = 0x01;
inline constexpr std::uint8_t galapagos_address = 0x02;
inline constexpr std::uint8_t adelie_address = 0x03;
inline constexpr std::uint8_t fairy_address_base = 0x10;
inline constexpr std::uint8_t discovery_address_base = 0x80;
inline constexpr std::uint8_t broadcast_address = 0xFF;

inline constexpr std::uint32_t common_timer_hz = 16'000'000;
inline constexpr std::uint32_t sync_rate_hz = 4;
inline constexpr std::uint32_t rs485_baud = 460800;

inline constexpr std::uint8_t fairy_address(std::size_t zero_based_slot) {
  return static_cast<std::uint8_t>(fairy_address_base + zero_based_slot);
}

inline constexpr std::uint8_t discovery_address(std::size_t zero_based_slot) {
  return static_cast<std::uint8_t>(discovery_address_base + zero_based_slot);
}

inline constexpr bool is_fairy_address(std::uint8_t address) {
  return address >= fairy_address_base &&
         address < fairy_address_base + max_fairies;
}

inline constexpr bool is_discovery_address(std::uint8_t address) {
  return address >= discovery_address_base &&
         address < discovery_address_base + max_fairies;
}

} // namespace fairy::config
