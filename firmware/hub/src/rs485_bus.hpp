#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "rig_protocol/transport.hpp"

namespace hub_rs485 {

struct Received {
  rig::transport::Channel channel{};
  std::uint8_t flags{};
  std::uint8_t source{};
  std::uint8_t destination{};
  std::uint16_t transfer_id{};
  std::uint16_t length{};
  std::array<std::uint8_t, rig::transport::max_message_size> payload{};
};

struct Diagnostics {
  std::uint32_t errors{};
  std::uint32_t retries{};
  std::uint32_t timeouts{};
  std::uint32_t decode_errors{};
  std::uint32_t reassembly_errors{};
  std::uint32_t transmit_errors{};
  // Receiver health. A receive stop is a UART error that halted reception
  // (framing, overrun, ...); the driver then restarts it from the callback.
  std::uint32_t receive_stops{};
  std::uint32_t receive_restart_failures{};
  // Full receiver resets made by the stuck-bus watchdog, and how many of
  // them were followed by valid frames again.
  std::uint32_t receiver_resets{};
  std::uint32_t receiver_recoveries{};
  // Resets where the driver never confirmed the stop and the receiver had
  // to be kicked in hardware.
  std::uint32_t receiver_kicks{};
  bool receiver_running{};
};

int initialize();

int exchange(std::uint8_t destination, rig::transport::Channel channel,
             const std::uint8_t *payload, std::size_t payload_length,
             Received &response, std::uint32_t timeout_ms = 4,
             std::uint8_t flags = rig::transport::ack_required,
             bool acknowledge_response = false);

int send_one_way(std::uint8_t destination, rig::transport::Channel channel,
                 std::uint16_t transfer_id, std::uint8_t flags,
                 const std::uint8_t *payload = nullptr,
                 std::size_t payload_length = 0);

std::uint32_t errors();
std::uint32_t retries();
Diagnostics diagnostics();

} // namespace hub_rs485
