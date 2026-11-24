#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "rig_protocol/discovery_protocol.hpp"

namespace node_rs485 {

void initialize();
void service();

/* False when the identity page is blank or corrupt; the board then stays
 * silent on RS485 and blinks red. */
bool has_identity();
const rig::protocol::DeviceUuid &uuid();
/* node_address(node id), or unassigned when has_identity() is false. */
std::uint8_t address();
/* The node id (0 to max_nodes - 1), or 0xFF without an identity. */
std::uint8_t logical_slot();
std::uint32_t transport_errors();
std::uint32_t duplicate_frames();
std::uint64_t last_contact_ticks();

} // namespace node_rs485
