#pragma once

#include <cstdint>

#include "fairy_shared/clock_model.hpp"
#include "fairy_shared/transport.hpp"

namespace korora_galapagos {

void initialize();
void connected();
void disconnected();
void central_anchor(std::uint16_t event_counter,
                    std::uint64_t controller_ticks);
void receive(const fairy::transport::MessageView &message);

bool korora_to_local(std::uint64_t korora_ticks, std::uint64_t &local_ticks);
bool local_to_korora(std::uint64_t local_ticks, std::uint64_t &korora_ticks);
fairy::time::ClockQuality quality();

} // namespace korora_galapagos
