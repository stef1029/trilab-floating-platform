#pragma once

#include <cstdint>

#include "rig_protocol/clock_model.hpp"

namespace hub_controller_clock {

int initialize();
bool to_hub(std::uint64_t controller_ticks, std::uint64_t &hub_ticks);
rig::time::ClockQuality quality();

} // namespace hub_controller_clock
