#pragma once

#include <cstdint>

#include "rig_protocol/transport.hpp"

namespace hub_control {

void initialize();
void receive_from_host(const rig::transport::MessageView &message,
                         std::uint64_t receive_ticks);
void host_disconnected();
void dependency_disconnected(const char *reason);
std::uint32_t active_session();

} // namespace hub_control
