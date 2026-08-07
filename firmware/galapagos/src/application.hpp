#pragma once

#include <cstddef>
#include <cstdint>

namespace galapagos_application {

void initialize();
std::size_t handle_command(const std::uint8_t *message, std::size_t length,
                           std::uint8_t *response, std::size_t capacity);
void anchor_observation(std::uint16_t event_counter, std::uint64_t anchor_us);
void connected();
void disconnected();

} // namespace galapagos_application
