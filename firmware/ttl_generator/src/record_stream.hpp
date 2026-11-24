#pragma once

#include <cstddef>
#include <cstdint>

#include "rig_protocol/record_protocol.hpp"
#include "rig_protocol/transport.hpp"

struct bt_conn;
struct bt_gatt_attr;

namespace ttl_generator_stream {

void initialize();
void set_connection(bt_conn *connection, const bt_gatt_attr *notify_attribute);
void clear_connection();

bool publish_record(rig::protocol::RecordType type, std::uint16_t flags,
                    std::uint64_t ticks, std::uint32_t session_id,
                    const std::uint8_t *payload = nullptr,
                    std::size_t payload_length = 0);

bool publish_application(rig::transport::Channel channel,
                         std::uint8_t destination, std::uint16_t transfer_id,
                         std::uint8_t flags, const std::uint8_t *payload,
                         std::size_t payload_length);

std::uint32_t dropped();
std::uint32_t transport_errors();

} // namespace ttl_generator_stream
