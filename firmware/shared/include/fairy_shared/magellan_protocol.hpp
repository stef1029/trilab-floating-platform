#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fairy::protocol {

inline constexpr std::uint16_t magellan_magic = 0x4D47;
inline constexpr std::uint8_t magellan_version = 1;
inline constexpr std::size_t magellan_header_size = 12;
inline constexpr std::size_t uuid_size = 12;
inline constexpr std::size_t discovery_slot_count = 16;
inline constexpr std::uint16_t discovery_slot_us = 2500;

using DeviceUuid = std::array<std::uint8_t, uuid_size>;

enum class MagellanType : std::uint8_t {
  discover = 1,
  offer = 2,
  assign = 3,
  assigned = 4,
  release = 5,
  verify = 6,
  verified = 7,
};

enum Capability : std::uint32_t {
  capability_light_gate = 1U << 0U,
  capability_rgb = 1U << 1U,
  capability_audio = 1U << 2U,
  capability_valve = 1U << 3U,
  capability_ir = 1U << 4U,
  capability_sync_capture = 1U << 5U,
};

struct MagellanHeader {
  MagellanType type{MagellanType::discover};
  std::uint32_t nonce{};
  std::uint16_t payload_length{};
};

struct MagellanView {
  MagellanHeader header{};
  const std::uint8_t *payload{};
};

struct DiscoveryParameters {
  std::uint8_t slot_count{discovery_slot_count};
  std::uint16_t slot_us{discovery_slot_us};
  std::uint8_t round{};
};

struct Offer {
  DeviceUuid uuid{};
  std::uint32_t capabilities{};
  std::uint32_t boot_count{};
};

struct Assignment {
  DeviceUuid uuid{};
  std::uint8_t address{};
  std::uint8_t logical_slot{};
};

std::size_t encode_magellan(const MagellanHeader &header,
                            const std::uint8_t *payload,
                            std::size_t payload_length,
                            std::uint8_t *destination, std::size_t capacity);

bool decode_magellan(const std::uint8_t *message, std::size_t length,
                     MagellanView &output);

std::uint8_t discovery_slot(const DeviceUuid &uuid, std::uint32_t nonce,
                            std::uint8_t slot_count);

std::size_t encode_offer(const Offer &offer, std::uint8_t *destination,
                         std::size_t capacity);
bool decode_offer(const std::uint8_t *payload, std::size_t length,
                  Offer &output);

std::size_t encode_assignment(const Assignment &assignment,
                              std::uint8_t *destination, std::size_t capacity);
bool decode_assignment(const std::uint8_t *payload, std::size_t length,
                       Assignment &output);

} // namespace fairy::protocol
