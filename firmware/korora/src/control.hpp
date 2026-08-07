#pragma once

#include <cstdint>

#include "fairy_shared/transport.hpp"

namespace korora_control {

void initialize();
void receive_from_adelie(const fairy::transport::MessageView &message,
                         std::uint64_t receive_ticks);
void adelie_disconnected();
void dependency_disconnected(const char *reason);
std::uint32_t active_session();

} // namespace korora_control
