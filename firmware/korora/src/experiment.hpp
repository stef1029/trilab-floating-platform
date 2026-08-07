#pragma once

#include <cstdint>

#include "fairy_shared/adelie_protocol.hpp"

namespace korora_experiment {

void initialize();
void adelie_connected();
void publish_fault(const char *reason);
void set_session(std::uint32_t session_id);

bool start_ttl_train(std::uint32_t frequency_millihz, std::uint32_t width_us,
                     std::uint32_t count);
void stop_ttl_train();

bool start_sync_test(std::uint32_t command_interval_ms, std::uint32_t width_us);
void stop_sync_test();
bool sync_test_active();

void note_galapagos_ttl(std::uint32_t sequence,
                        std::uint64_t local_actual_ticks,
                        std::uint64_t korora_actual_ticks);
void note_galapagos_ttl_status(std::uint32_t sequence,
                               fairy::protocol::Status status);

void record_transport_timing(std::uint16_t transfer_id,
                             std::uint32_t command_id, std::uint16_t operation,
                             std::uint64_t receive_ticks,
                             std::uint64_t queued_ticks,
                             std::uint64_t transmit_start_ticks,
                             std::uint64_t transmit_complete_ticks,
                             std::uint16_t fragment_count,
                             std::uint16_t att_mtu);

} // namespace korora_experiment
