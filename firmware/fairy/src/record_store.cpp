#include "record_store.hpp"

#include "fairy_shared/system_config.hpp"

namespace fairy_records {
namespace {

fairy::StaticQueue<StoredRecord, 48> queue;
std::uint32_t record_id;
std::uint32_t dropped_count;

} // namespace

void initialize() {
  queue.clear();
  record_id = 0;
  dropped_count = 0;
}

bool enqueue(fairy::protocol::RecordType type, std::uint16_t flags,
             std::uint64_t timestamp_ticks, std::uint32_t session_id,
             const std::uint8_t *payload, std::size_t payload_length) {
  StoredRecord record;
  fairy::protocol::RecordHeader header;
  header.type = type;
  header.flags = flags;
  header.record_id = ++record_id;
  header.session_id = session_id;
  header.timestamp_ticks = timestamp_ticks;
  header.clock_hz = fairy::config::common_timer_hz;
  const std::size_t length =
      fairy::protocol::encode_record(header, payload, payload_length,
                                     record.bytes.data(), record.bytes.size());
  if (length == 0U) {
    ++dropped_count;
    return false;
  }
  record.length = static_cast<std::uint16_t>(length);
  if (!queue.push(record)) {
    ++dropped_count;
    return false;
  }
  return true;
}

const StoredRecord *front() { return queue.front(); }
bool pop() { return queue.drop_front(); }
std::size_t size() { return queue.size(); }
std::uint32_t dropped() { return dropped_count; }
std::uint32_t next_id() { return record_id + 1U; }

} // namespace fairy_records
