#pragma once

#include <cstddef>
#include <cstdint>

namespace fairy_application {

void initialize(std::uint32_t reset_cause, bool light_test_passed,
                std::uint16_t dark_adc, std::uint16_t clear_adc);

std::size_t handle_adelie(const std::uint8_t *message, std::size_t length,
                          std::uint8_t *response, std::size_t capacity);

void service();
std::uint32_t session_id();
bool session_active();

} // namespace fairy_application
