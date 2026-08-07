#pragma once

#include <cstddef>
#include <cstdint>

#include "fairy_shared/transport.hpp"

namespace korora_ble {

int initialize();

// Use diagnostic_priority for time-critical Fairy records. They share the
// reserved Adelie event queue and can preempt bulk telemetry between fragments.
bool send_to_adelie(std::uint8_t source, fairy::transport::Channel channel,
                    const std::uint8_t *payload, std::size_t payload_length,
                    std::uint16_t transfer_id = 0, std::uint8_t flags = 0,
                    std::uint64_t command_receive_ticks = 0,
                    bool diagnostic_priority = false);

bool send_to_galapagos(fairy::transport::Channel channel,
                       const std::uint8_t *payload, std::size_t payload_length,
                       std::uint16_t transfer_id = 0,
                       std::uint8_t flags = fairy::transport::ack_required);

bool adelie_connected();
bool galapagos_connected();
std::int8_t galapagos_rssi();
std::uint32_t dropped_to_adelie();
std::uint32_t dropped_to_galapagos();

} // namespace korora_ble
