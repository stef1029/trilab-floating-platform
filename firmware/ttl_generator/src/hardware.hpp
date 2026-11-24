#pragma once

#include <cstdint>

#include "rig_protocol/command_protocol.hpp"

namespace ttl_generator_hardware {

int initialize();
std::uint64_t now_ticks();
void set_session(std::uint32_t session_id);

rig::protocol::Status schedule_ttl(std::uint32_t sequence,
                                     std::uint64_t target_ticks,
                                     std::uint32_t width_us);
void force_ttl_low();
bool ttl_armed();

} // namespace ttl_generator_hardware
