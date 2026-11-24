#pragma once

#include <cstddef>
#include <cstdint>

/*
 * Change this one value when building a rig with a different maximum number
 * of node boards. The prototype can be built with -DNODE_MAX_BOARDS=2.
 */
#ifndef RIG_MAX_NODES
#define RIG_MAX_NODES 5
#endif

#ifndef NODE_ENABLE_DEBUG_STREAM
#define NODE_ENABLE_DEBUG_STREAM 1
#endif

namespace rig::config {

inline constexpr std::size_t max_nodes = RIG_MAX_NODES;
static_assert(max_nodes >= 1 && max_nodes <= 14,
              "RIG_MAX_NODES must be between 1 and 14");

inline constexpr std::uint8_t unassigned_address = 0x00;
inline constexpr std::uint8_t hub_address = 0x01;
inline constexpr std::uint8_t ttl_generator_address = 0x02;
inline constexpr std::uint8_t host_address = 0x03;
inline constexpr std::uint8_t node_address_base = 0x10;
inline constexpr std::uint8_t discovery_address_base = 0x80;
inline constexpr std::uint8_t broadcast_address = 0xFF;

inline constexpr std::uint32_t common_timer_hz = 16'000'000;
inline constexpr std::uint32_t sync_rate_hz = 4;
inline constexpr std::uint32_t rs485_baud = 460800;

inline constexpr std::uint8_t node_address(std::size_t zero_based_slot) {
  return static_cast<std::uint8_t>(node_address_base + zero_based_slot);
}

inline constexpr std::uint8_t discovery_address(std::size_t zero_based_slot) {
  return static_cast<std::uint8_t>(discovery_address_base + zero_based_slot);
}

inline constexpr bool is_node_address(std::uint8_t address) {
  return address >= node_address_base &&
         address < node_address_base + max_nodes;
}

inline constexpr bool is_discovery_address(std::uint8_t address) {
  return address >= discovery_address_base &&
         address < discovery_address_base + max_nodes;
}

} // namespace rig::config
