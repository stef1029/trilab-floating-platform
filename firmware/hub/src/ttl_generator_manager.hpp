#pragma once

#include <cstdint>

#include "rig_protocol/clock_model.hpp"
#include "rig_protocol/transport.hpp"

namespace hub_ttl_generator {

void initialize();
void connected();
void disconnected();
void central_anchor(std::uint16_t event_counter,
                    std::uint64_t controller_ticks);
void receive(const rig::transport::MessageView &message);

bool hub_to_local(std::uint64_t hub_ticks, std::uint64_t &local_ticks);
bool local_to_hub(std::uint64_t local_ticks, std::uint64_t &hub_ticks);
rig::time::ClockQuality quality();

} // namespace hub_ttl_generator
