#include "record_stream.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "debug_log.hpp"
#include "fairy_shared/system_config.hpp"

namespace galapagos_stream {
namespace {

struct Outbound {
  fairy::transport::Channel channel{};
  std::uint8_t destination{};
  std::uint8_t flags{};
  std::uint16_t transfer_id{};
  std::uint16_t length{};
  std::array<std::uint8_t, fairy::transport::max_message_size> payload{};
};

K_MSGQ_DEFINE(control_outbound_queue, sizeof(Outbound), 8, 4);
K_MSGQ_DEFINE(record_outbound_queue, sizeof(Outbound), 48, 4);
K_SEM_DEFINE(outbound_ready, 0, 1);
K_MUTEX_DEFINE(connection_mutex);

struct NotificationSlot {
  atomic_t in_use{};
  bt_gatt_notify_params params{};
  std::array<std::uint8_t, fairy::transport::max_raw_frame_size> frame{};
};

std::array<NotificationSlot, 2> notification_slots{};
K_SEM_DEFINE(notification_slot_available, 2, 2);

bt_conn *active_connection;
const bt_gatt_attr *active_attribute;
atomic_t dropped_count;
atomic_t error_count;
atomic_t record_id;
atomic_t next_transfer = ATOMIC_INIT(1);

void release_notification_slot(NotificationSlot *slot) {
  if (slot == nullptr) {
    return;
  }
  atomic_clear(&slot->in_use);
  k_sem_give(&notification_slot_available);
}

void notify_complete(bt_conn *, void *user_data) {
  release_notification_slot(static_cast<NotificationSlot *>(user_data));
}

void reset_notification_slots() {
  k_sem_reset(&notification_slot_available);
  for (NotificationSlot &slot : notification_slots) {
    atomic_clear(&slot.in_use);
  }
  for (std::size_t index = 0; index < notification_slots.size(); ++index) {
    k_sem_give(&notification_slot_available);
  }
}

NotificationSlot *acquire_notification_slot() {
  if (k_sem_take(&notification_slot_available, K_MSEC(75)) != 0) {
    return nullptr;
  }
  for (NotificationSlot &slot : notification_slots) {
    if (atomic_cas(&slot.in_use, 0, 1)) {
      return &slot;
    }
  }
  k_sem_give(&notification_slot_available);
  return nullptr;
}

bool queue_notification(bt_conn *connection, const bt_gatt_attr *attribute,
                        const std::uint8_t *frame, std::size_t length) {
  if (length > fairy::transport::max_raw_frame_size) {
    return false;
  }
  NotificationSlot *slot = acquire_notification_slot();
  if (slot == nullptr) {
    return false;
  }
  std::memcpy(slot->frame.data(), frame, length);
  slot->params = {};
  slot->params.attr = attribute;
  slot->params.data = slot->frame.data();
  slot->params.len = static_cast<std::uint16_t>(length);
  slot->params.func = notify_complete;
  slot->params.user_data = slot;

  int error = -EAGAIN;
  for (unsigned int attempt = 0; attempt < 50U; ++attempt) {
    error = bt_gatt_notify_cb(connection, &slot->params);
    if (error == -ENOMEM || error == -EAGAIN) {
      k_sleep(K_MSEC(2));
      continue;
    }
    break;
  }
  if (error != 0) {
    release_notification_slot(slot);
  }
  return error == 0;
}

void notification_thread(void *, void *, void *) {
  Outbound outbound;
  std::array<std::uint8_t, fairy::transport::max_raw_frame_size> frame{};

  while (true) {
    if (k_msgq_get(&control_outbound_queue, &outbound, K_NO_WAIT) != 0 &&
        k_msgq_get(&record_outbound_queue, &outbound, K_NO_WAIT) != 0) {
      k_sem_take(&outbound_ready, K_FOREVER);
      continue;
    }

    k_mutex_lock(&connection_mutex, K_FOREVER);
    bt_conn *connection =
        active_connection == nullptr ? nullptr : bt_conn_ref(active_connection);
    const bt_gatt_attr *attribute = active_attribute;
    k_mutex_unlock(&connection_mutex);
    if (connection == nullptr || attribute == nullptr) {
      atomic_inc(&dropped_count);
      continue;
    }

    const std::uint16_t mtu = bt_gatt_get_mtu(connection);
    const std::size_t fragment_capacity = std::max<std::size_t>(
        1U, std::min<std::size_t>(fairy::transport::max_fragment_payload,
                                  mtu > 17U ? mtu - 17U : 1U));
    const std::size_t fragment_count =
        outbound.length == 0U
            ? 1U
            : (outbound.length + fragment_capacity - 1U) / fragment_capacity;

    bool failed = fragment_count > 255U;
    for (std::size_t index = 0; !failed && index < fragment_count; ++index) {
      const std::size_t offset = index * fragment_capacity;
      const std::size_t chunk =
          outbound.length == 0U
              ? 0U
              : std::min(fragment_capacity,
                         static_cast<std::size_t>(outbound.length) - offset);
      fairy::transport::Header header;
      header.channel = outbound.channel;
      header.flags = outbound.flags;
      if (index == 0U) {
        header.flags |= fairy::transport::first_fragment;
      }
      if (index + 1U == fragment_count) {
        header.flags |= fairy::transport::last_fragment;
      }
      header.source = fairy::config::galapagos_address;
      header.destination = outbound.destination;
      header.fragment_index = static_cast<std::uint8_t>(index);
      header.fragment_count = static_cast<std::uint8_t>(fragment_count);
      header.transfer_id = outbound.transfer_id;

      const std::size_t frame_length = fairy::transport::encode_frame(
          header, chunk == 0U ? nullptr : outbound.payload.data() + offset,
          chunk, frame.data(), frame.size());
      if (frame_length == 0U) {
        failed = true;
        break;
      }

      if (!queue_notification(connection, attribute, frame.data(),
                              frame_length)) {
        failed = true;
      }
    }

    if (failed) {
      atomic_inc(&error_count);
    }
    bt_conn_unref(connection);
  }
}

K_THREAD_DEFINE(notification_thread_id, 3072, notification_thread, nullptr,
                nullptr, nullptr, 6, 0, 0);

} // namespace

void initialize() {
  atomic_set(&dropped_count, 0);
  atomic_set(&error_count, 0);
  atomic_set(&record_id, 0);
}

void set_connection(bt_conn *connection, const bt_gatt_attr *notify_attribute) {
  k_mutex_lock(&connection_mutex, K_FOREVER);
  if (active_connection != nullptr) {
    bt_conn_unref(active_connection);
  }
  active_connection = connection == nullptr ? nullptr : bt_conn_ref(connection);
  active_attribute = notify_attribute;
  k_mutex_unlock(&connection_mutex);
}

void clear_connection() {
  k_mutex_lock(&connection_mutex, K_FOREVER);
  if (active_connection != nullptr) {
    bt_conn_unref(active_connection);
    active_connection = nullptr;
  }
  active_attribute = nullptr;
  k_mutex_unlock(&connection_mutex);
  k_msgq_purge(&control_outbound_queue);
  k_msgq_purge(&record_outbound_queue);
  k_sem_reset(&outbound_ready);
  reset_notification_slots();
}

bool publish_record(fairy::protocol::RecordType type, std::uint16_t flags,
                    std::uint64_t ticks, std::uint32_t session_id,
                    const std::uint8_t *payload, std::size_t payload_length) {
  Outbound message;
  message.channel = fairy::transport::Channel::fairy;
  message.destination = fairy::config::korora_address;
  message.transfer_id =
      static_cast<std::uint16_t>(atomic_inc(&next_transfer) + 1);
  fairy::protocol::RecordHeader header;
  header.type = type;
  header.flags = flags;
  header.record_id = static_cast<std::uint32_t>(atomic_inc(&record_id) + 1);
  header.session_id = session_id;
  header.timestamp_ticks = ticks;
  header.clock_hz = fairy::config::common_timer_hz;
  const std::size_t length = fairy::protocol::encode_record(
      header, payload, payload_length, message.payload.data(),
      message.payload.size());
  if (length == 0U) {
    atomic_inc(&dropped_count);
    return false;
  }
  message.length = static_cast<std::uint16_t>(length);
  if (k_msgq_put(&record_outbound_queue, &message, K_NO_WAIT) != 0) {
    atomic_inc(&dropped_count);
    return false;
  }
  k_sem_give(&outbound_ready);
  return true;
}

bool publish_application(fairy::transport::Channel channel,
                         std::uint8_t destination, std::uint16_t transfer_id,
                         std::uint8_t flags, const std::uint8_t *payload,
                         std::size_t payload_length) {
  if (payload_length > fairy::transport::max_message_size ||
      (payload_length != 0U && payload == nullptr)) {
    return false;
  }
  Outbound message;
  message.channel = channel;
  message.destination = destination;
  message.transfer_id = transfer_id;
  message.flags = flags;
  message.length = static_cast<std::uint16_t>(payload_length);
  if (payload_length != 0U) {
    std::memcpy(message.payload.data(), payload, payload_length);
  }
  if (k_msgq_put(&control_outbound_queue, &message, K_NO_WAIT) != 0) {
    atomic_inc(&dropped_count);
    return false;
  }
  k_sem_give(&outbound_ready);
  return true;
}

std::uint32_t dropped() {
  return static_cast<std::uint32_t>(atomic_get(&dropped_count));
}

std::uint32_t transport_errors() {
  return static_cast<std::uint32_t>(atomic_get(&error_count));
}

} // namespace galapagos_stream
