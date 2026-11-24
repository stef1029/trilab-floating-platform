#pragma once

#include <cstddef>
#include <cstdint>

#include "rig_protocol/transport.hpp"

namespace hub_ble {

int initialize();

// Use diagnostic_priority for time-critical records. They share the
// reserved Host event queue and can preempt bulk telemetry between fragments.
bool send_to_host(std::uint8_t source, rig::transport::Channel channel,
                    const std::uint8_t *payload, std::size_t payload_length,
                    std::uint16_t transfer_id = 0, std::uint8_t flags = 0,
                    std::uint64_t command_receive_ticks = 0,
                    bool diagnostic_priority = false);

bool send_to_ttl_generator(rig::transport::Channel channel,
                       const std::uint8_t *payload, std::size_t payload_length,
                       std::uint16_t transfer_id = 0,
                       std::uint8_t flags = rig::transport::ack_required);

bool host_connected();
bool ttl_generator_connected();
std::int8_t ttl_generator_rssi();
std::uint32_t dropped_to_host();
std::uint32_t dropped_to_ttl_generator();

} // namespace hub_ble
