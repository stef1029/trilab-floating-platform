#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

/*
 * Discovery channel codec.
 *
 * Node boards no longer join the bus through discovery: each is numbered
 * when flashed (node_identity.hpp) and always uses node_address(number).
 * The only message still sent on this channel is the hub's boot-time
 * release, which tells every node to make its outputs safe. The other
 * message types are kept only so their numbers are not reused.
 *
 * The UUID type and capability bits also live here; both are used by the
 * inventory the hub reports to Adelie.
 */

namespace rig::protocol {

inline constexpr std::uint16_t discovery_magic = 0x4D47;
inline constexpr std::uint8_t discovery_version = 1;
inline constexpr std::size_t discovery_header_size = 12;
inline constexpr std::size_t uuid_size = 12;

using DeviceUuid = std::array<std::uint8_t, uuid_size>;

enum class DiscoveryType : std::uint8_t {
  discover = 1, // retired
  offer = 2,    // retired
  assign = 3,   // retired
  assigned = 4, // retired
  release = 5,  // hub restarted: nodes make outputs safe
  verify = 6,   // reserved
  verified = 7, // reserved
};

enum Capability : std::uint32_t {
  capability_light_gate = 1U << 0U,
  capability_rgb = 1U << 1U,
  capability_audio = 1U << 2U,
  capability_valve = 1U << 3U,
  capability_ir = 1U << 4U,
  capability_sync_capture = 1U << 5U,
  capability_green_blue_cue = 1U << 6U,
  capability_spotlight = 1U << 7U,
  capability_load_cell = 1U << 8U,
};

struct DiscoveryHeader {
  DiscoveryType type{DiscoveryType::discover};
  std::uint32_t nonce{};
  std::uint16_t payload_length{};
};

struct DiscoveryView {
  DiscoveryHeader header{};
  const std::uint8_t *payload{};
};

std::size_t encode_discovery(const DiscoveryHeader &header,
                            const std::uint8_t *payload,
                            std::size_t payload_length,
                            std::uint8_t *destination, std::size_t capacity);

bool decode_discovery(const std::uint8_t *message, std::size_t length,
                     DiscoveryView &output);

} // namespace rig::protocol
