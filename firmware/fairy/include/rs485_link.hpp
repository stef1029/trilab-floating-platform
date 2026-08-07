#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "fairy_shared/magellan_protocol.hpp"

namespace fairy_rs485 {

void initialize();
void service();

const fairy::protocol::DeviceUuid &uuid();
std::uint8_t address();
std::uint8_t logical_slot();
std::uint32_t transport_errors();
std::uint32_t duplicate_frames();
std::uint64_t last_contact_ticks();

} // namespace fairy_rs485
