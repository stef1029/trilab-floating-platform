#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "fairy_shared/magellan_protocol.hpp"
#include "fairy_shared/transport.hpp"

namespace korora_rs485 {

struct Received {
  fairy::transport::Channel channel{};
  std::uint8_t flags{};
  std::uint8_t source{};
  std::uint8_t destination{};
  std::uint16_t transfer_id{};
  std::uint16_t length{};
  std::array<std::uint8_t, fairy::transport::max_message_size> payload{};
};

struct DiscoveredOffer {
  fairy::protocol::Offer offer{};
  std::uint16_t transfer_id{};
};

struct Diagnostics {
  std::uint32_t errors{};
  std::uint32_t retries{};
  std::uint32_t timeouts{};
  std::uint32_t decode_errors{};
  std::uint32_t reassembly_errors{};
  std::uint32_t transmit_errors{};
};

int initialize();

int exchange(std::uint8_t destination, fairy::transport::Channel channel,
             const std::uint8_t *payload, std::size_t payload_length,
             Received &response, std::uint32_t timeout_ms = 4,
             std::uint8_t flags = fairy::transport::ack_required,
             bool acknowledge_response = false);

int send_one_way(std::uint8_t destination, fairy::transport::Channel channel,
                 std::uint16_t transfer_id, std::uint8_t flags,
                 const std::uint8_t *payload = nullptr,
                 std::size_t payload_length = 0);

std::size_t discover(std::uint32_t nonce, std::uint8_t round,
                     DiscoveredOffer *offers, std::size_t capacity);

std::uint32_t errors();
std::uint32_t retries();
Diagnostics diagnostics();

} // namespace korora_rs485
