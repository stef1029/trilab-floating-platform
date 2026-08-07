#pragma once

#include <cstdint>

#include "fairy_shared/clock_model.hpp"

namespace korora_controller_clock {

int initialize();
bool to_korora(std::uint64_t controller_ticks, std::uint64_t &korora_ticks);
fairy::time::ClockQuality quality();

} // namespace korora_controller_clock
