#pragma once

#include <cstdint>

namespace korora_time {

int initialize();
std::uint64_t now();
std::uint32_t pulse_count();
std::uint64_t pulse_ticks(std::uint32_t pulse);

} // namespace korora_time
