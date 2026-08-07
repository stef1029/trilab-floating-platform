#include "ble_gateway.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/atomic.h>

#include "control.hpp"
#include "controller_clock.hpp"
#include "debug_log.hpp"
#include "experiment.hpp"
#include "fairy_shared/adelie_protocol.hpp"
#include "fairy_shared/sdc_anchor_report.h"
#include "fairy_shared/system_config.hpp"
#include "galapagos_manager.hpp"
#include "timebase.hpp"

namespace korora_ble {
namespace {

#define BT_UUID_ADELIE_SERVICE_VAL                                             \
  BT_UUID_128_ENCODE(0xA88279D0, 0x7009, 0x4BEE, 0xA6F8, 0xE1DC3FF02B92)
#define BT_UUID_ADELIE_RX_VAL                                                  \
  BT_UUID_128_ENCODE(0xA88279D1, 0x7009, 0x4BEE, 0xA6F8, 0xE1DC3FF02B92)
#define BT_UUID_ADELIE_TX_VAL                                                  \
  BT_UUID_128_ENCODE(0xA88279D2, 0x7009, 0x4BEE, 0xA6F8, 0xE1DC3FF02B92)

#define BT_UUID_GALAPAGOS_SERVICE_VAL                                          \
  BT_UUID_128_ENCODE(0xA88278D0, 0x7009, 0x4BEE, 0xA6F8, 0xE1DC3FF02B92)
#define BT_UUID_GALAPAGOS_RX_VAL                                               \
  BT_UUID_128_ENCODE(0xA88278D1, 0x7009, 0x4BEE, 0xA6F8, 0xE1DC3FF02B92)
#define BT_UUID_GALAPAGOS_TX_VAL                                               \
  BT_UUID_128_ENCODE(0xA88278D2, 0x7009, 0x4BEE, 0xA6F8, 0xE1DC3FF02B92)

const bt_uuid_128 adelie_service_uuid =
    BT_UUID_INIT_128(BT_UUID_ADELIE_SERVICE_VAL);
const bt_uuid_128 adelie_rx_uuid = BT_UUID_INIT_128(BT_UUID_ADELIE_RX_VAL);
const bt_uuid_128 adelie_tx_uuid = BT_UUID_INIT_128(BT_UUID_ADELIE_TX_VAL);
const bt_uuid_128 galapagos_service_uuid =
    BT_UUID_INIT_128(BT_UUID_GALAPAGOS_SERVICE_VAL);
const bt_uuid_128 galapagos_rx_uuid =
    BT_UUID_INIT_128(BT_UUID_GALAPAGOS_RX_VAL);
const bt_uuid_128 galapagos_tx_uuid =
    BT_UUID_INIT_128(BT_UUID_GALAPAGOS_TX_VAL);

ssize_t host_write(bt_conn *, const bt_gatt_attr *, const void *, std::uint16_t,
                   std::uint16_t, std::uint8_t);
void host_subscription(const bt_gatt_attr *, std::uint16_t);

BT_GATT_SERVICE_DEFINE(
    adelie_service, BT_GATT_PRIMARY_SERVICE(&adelie_service_uuid),
    BT_GATT_CHARACTERISTIC(&adelie_rx_uuid.uuid,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, nullptr, host_write, nullptr),
    BT_GATT_CHARACTERISTIC(&adelie_tx_uuid.uuid, BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE, nullptr, nullptr, nullptr),
    BT_GATT_CCC(host_subscription, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

enum class Endpoint : std::uint8_t {
  adelie,
  galapagos,
};

struct Outbound {
  Endpoint endpoint{};
  fairy::transport::Channel channel{};
  std::uint8_t source{};
  std::uint8_t destination{};
  std::uint8_t flags{};
  std::uint16_t transfer{};
  std::uint16_t length{};
  std::uint64_t command_receive_ticks{};
  std::uint64_t queued_ticks{};
  std::uint64_t transmit_start_ticks{};
  std::uint32_t command_id{};
  std::uint32_t connection_generation{};
  std::uint16_t operation{};
  std::uint16_t mtu{};
  std::uint16_t fragment_capacity{};
  std::uint16_t fragment_count{};
  std::uint16_t next_fragment{};
  std::array<std::uint8_t, fairy::transport::max_message_size> payload{};
};

K_MSGQ_DEFINE(adelie_outbound_queue, sizeof(Outbound), 96, 4);
K_MSGQ_DEFINE(adelie_control_outbound_queue, sizeof(Outbound), 16, 4);
K_MSGQ_DEFINE(adelie_diagnostic_outbound_queue, sizeof(Outbound), 16, 4);
K_MSGQ_DEFINE(adelie_resume_outbound_queue, sizeof(Outbound), 1, 4);
K_MSGQ_DEFINE(galapagos_outbound_queue, sizeof(Outbound), 16, 4);
K_MUTEX_DEFINE(connection_mutex);
K_MUTEX_DEFINE(send_mutex);
K_MUTEX_DEFINE(command_trace_mutex);

/*
 * A notification buffer must remain valid until Zephyr calls its completion
 * function. Two slots allow useful pipelining without putting a long train of
 * telemetry ahead of interactive replies or consuming the controller buffers
 * needed by the Galapagos link.
 */
struct NotificationSlot {
  atomic_t in_use{};
  bt_gatt_notify_params params{};
  std::array<std::uint8_t, fairy::transport::max_raw_frame_size> frame{};
};

std::array<NotificationSlot, 2> notification_slots{};
K_SEM_DEFINE(notification_slot_available, 2, 2);

bt_conn *host_connection;
bt_conn *galapagos_connection;
atomic_t host_notifications;
atomic_t galapagos_active;
atomic_t galapagos_connecting;
atomic_t transfer_counter = ATOMIC_INIT(1);
atomic_t last_galapagos_rssi = ATOMIC_INIT(-127);
atomic_t adelie_dropped;
atomic_t galapagos_dropped;
atomic_t low_mtu_timing_records;
atomic_t adelie_connection_generation;
atomic_t galapagos_connection_generation;

fairy::transport::Reassembler host_reassembler;
fairy::transport::Reassembler galapagos_reassembler;

struct CommandTrace {
  std::uint16_t transfer{};
  std::uint64_t receive_ticks{};
  bool used{};
};

std::array<CommandTrace, 32> command_traces{};

void remember_command_receive(std::uint16_t transfer,
                              std::uint64_t receive_ticks) {
  k_mutex_lock(&command_trace_mutex, K_FOREVER);
  CommandTrace &trace = command_traces[transfer % command_traces.size()];
  trace.transfer = transfer;
  trace.receive_ticks = receive_ticks;
  trace.used = true;
  k_mutex_unlock(&command_trace_mutex);
}

std::uint64_t take_command_receive(std::uint16_t transfer) {
  std::uint64_t ticks = 0;
  k_mutex_lock(&command_trace_mutex, K_FOREVER);
  CommandTrace &trace = command_traces[transfer % command_traces.size()];
  if (trace.used && trace.transfer == transfer) {
    ticks = trace.receive_ticks;
    trace.used = false;
  }
  k_mutex_unlock(&command_trace_mutex);
  return ticks;
}

void clear_command_traces() {
  k_mutex_lock(&command_trace_mutex, K_FOREVER);
  for (CommandTrace &trace : command_traces) {
    trace.used = false;
  }
  k_mutex_unlock(&command_trace_mutex);
}

enum class DiscoveryStage {
  none,
  service,
  rx,
  tx,
  ccc,
};

struct GalapagosClient {
  DiscoveryStage stage{DiscoveryStage::none};
  std::uint16_t service_end{};
  std::uint16_t rx_handle{};
  std::uint16_t tx_handle{};
  bt_gatt_discover_params discover{};
  bt_gatt_subscribe_params subscribe{};
  bt_gatt_exchange_params exchange{};
};

GalapagosClient client;

bt_le_scan_param make_galapagos_scan_parameters() {
  bt_le_scan_param parameters{};
  parameters.type = BT_LE_SCAN_TYPE_PASSIVE;
  parameters.options = BT_LE_SCAN_OPT_FILTER_DUPLICATE;
  /* 10 ms of scanning in each 100 ms interval. */
  parameters.interval = 160U;
  parameters.window = 16U;
  parameters.timeout = 0U;
  return parameters;
}

const bt_le_scan_param galapagos_scan_parameters =
    make_galapagos_scan_parameters();

std::uint16_t allocate_transfer() {
  const std::uint16_t value =
      static_cast<std::uint16_t>(atomic_inc(&transfer_counter) + 1);
  return value == 0U ? 1U : value;
}

std::uint32_t connection_generation(Endpoint endpoint) {
  return static_cast<std::uint32_t>(atomic_get(
      endpoint == Endpoint::adelie ? &adelie_connection_generation
                                   : &galapagos_connection_generation));
}

void release_notification_slot(NotificationSlot *slot) {
  if (slot == nullptr) {
    return;
  }
  atomic_clear(&slot->in_use);
  k_sem_give(&notification_slot_available);
}

void notification_complete(bt_conn *, void *user_data) {
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
  /* The semaphore and slot state should move together. Recover if they do not.
   */
  k_sem_give(&notification_slot_available);
  return nullptr;
}

bool send_notification(bt_conn *connection, const std::uint8_t *frame,
                       std::size_t length) {
  if (length > fairy::transport::max_raw_frame_size) {
    return false;
  }
  NotificationSlot *slot = acquire_notification_slot();
  if (slot == nullptr) {
    return false;
  }
  std::memcpy(slot->frame.data(), frame, length);
  slot->params = {};
  slot->params.attr = &adelie_service.attrs[4];
  slot->params.data = slot->frame.data();
  slot->params.len = static_cast<std::uint16_t>(length);
  slot->params.func = notification_complete;
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

bool write_galapagos(bt_conn *connection, const std::uint8_t *frame,
                     std::size_t length) {
  int error = -EAGAIN;
  for (unsigned int attempt = 0; attempt < 50U; ++attempt) {
    error = bt_gatt_write_without_response(connection, client.rx_handle, frame,
                                           static_cast<std::uint16_t>(length),
                                           false);
    if (error == -ENOMEM || error == -EAGAIN) {
      k_sleep(K_MSEC(2));
      continue;
    }
    break;
  }
  return error == 0;
}

void tx_thread(void *, void *, void *) {
  Outbound outbound;
  std::array<std::uint8_t, fairy::transport::max_raw_frame_size> frame{};
  while (true) {
    /*
     * Galapagos commands have timing deadlines. Adelie responses must also
     * stay responsive. Bulk Adelie messages are sent one fragment at a time,
     * so these queues are checked again between their fragments.
     */
    bool bulk_message = false;
    if (k_msgq_get(&galapagos_outbound_queue, &outbound, K_NO_WAIT) != 0 &&
        k_msgq_get(&adelie_control_outbound_queue, &outbound, K_NO_WAIT) != 0 &&
        k_msgq_get(&adelie_diagnostic_outbound_queue, &outbound, K_NO_WAIT) !=
            0) {
      if (k_msgq_get(&adelie_resume_outbound_queue, &outbound, K_NO_WAIT) ==
          0) {
        bulk_message = true;
      } else if (k_msgq_get(&adelie_outbound_queue, &outbound, K_NO_WAIT) ==
                 0) {
        bulk_message = true;
      } else {
        k_sleep(K_MSEC(1));
        continue;
      }
    }

    if (outbound.connection_generation !=
        connection_generation(outbound.endpoint)) {
      continue;
    }

    k_mutex_lock(&connection_mutex, K_FOREVER);
    bt_conn *connection = nullptr;
    if (outbound.endpoint == Endpoint::adelie && host_connection != nullptr &&
        atomic_get(&host_notifications) != 0) {
      connection = bt_conn_ref(host_connection);
    } else if (outbound.endpoint == Endpoint::galapagos &&
               galapagos_connection != nullptr &&
               atomic_get(&galapagos_active) != 0 && client.rx_handle != 0U) {
      connection = bt_conn_ref(galapagos_connection);
    }
    k_mutex_unlock(&connection_mutex);
    if (connection == nullptr) {
      continue;
    }

    const std::uint16_t connection_mtu = bt_gatt_get_mtu(connection);
    if (outbound.fragment_count == 0U) {
      outbound.mtu = connection_mtu;
      outbound.fragment_capacity =
          static_cast<std::uint16_t>(std::max<std::size_t>(
              1U, std::min<std::size_t>(
                      fairy::transport::max_fragment_payload,
                      connection_mtu > 17U ? connection_mtu - 17U : 1U)));
      const std::size_t calculated_count =
          outbound.length == 0U
              ? 1U
              : (outbound.length + outbound.fragment_capacity - 1U) /
                    outbound.fragment_capacity;
      outbound.fragment_count =
          calculated_count <= 255U
              ? static_cast<std::uint16_t>(calculated_count)
              : 0U;
    }
    const std::size_t fragment_capacity = outbound.fragment_capacity;
    const std::size_t count = outbound.fragment_count;
    const std::size_t first_fragment = outbound.next_fragment;
    const std::size_t end_fragment =
        bulk_message ? std::min(first_fragment + 1U, count) : count;
    bool good = count != 0U && count <= 255U && first_fragment < count;

    k_mutex_lock(&send_mutex, K_FOREVER);
    if (outbound.transmit_start_ticks == 0U) {
      outbound.transmit_start_ticks = korora_time::now();
    }
    const std::uint64_t transmit_start_ticks = outbound.transmit_start_ticks;
    for (std::size_t index = first_fragment; good && index < end_fragment;
         ++index) {
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
      if (index + 1U == count) {
        header.flags |= fairy::transport::last_fragment;
      }
      header.source = outbound.source;
      header.destination = outbound.destination;
      header.fragment_index = static_cast<std::uint8_t>(index);
      header.fragment_count = static_cast<std::uint8_t>(count);
      header.transfer_id = outbound.transfer;
      const std::size_t frame_length = fairy::transport::encode_frame(
          header, chunk == 0U ? nullptr : outbound.payload.data() + offset,
          chunk, frame.data(), frame.size());
      good = frame_length != 0U &&
             (outbound.endpoint == Endpoint::adelie
                  ? send_notification(connection, frame.data(), frame_length)
                  : write_galapagos(connection, frame.data(), frame_length));
    }
    const std::uint64_t transmit_complete_ticks = korora_time::now();
    k_mutex_unlock(&send_mutex);
    bool paused = false;
    const bool cancelled = outbound.connection_generation !=
                           connection_generation(outbound.endpoint);
    if (good && !cancelled && bulk_message && end_fragment < count) {
      outbound.next_fragment = static_cast<std::uint16_t>(end_fragment);
      paused =
          k_msgq_put(&adelie_resume_outbound_queue, &outbound, K_NO_WAIT) == 0;
      good = paused;
    }
    if (!good && !cancelled) {
      atomic_inc(outbound.endpoint == Endpoint::adelie ? &adelie_dropped
                                                       : &galapagos_dropped);
      korora_debug::log("BLE_TX_ERROR endpoint=%u transfer=%u\r\n",
                        static_cast<unsigned int>(outbound.endpoint),
                        outbound.transfer);
    }
    if (!paused && !cancelled && outbound.command_receive_ticks != 0U) {
      const std::uint64_t receive_to_queue_us =
          (outbound.queued_ticks - outbound.command_receive_ticks) / 16U;
      const std::uint64_t queue_to_start_us =
          (transmit_start_ticks - outbound.queued_ticks) / 16U;
      const std::uint64_t transmit_us =
          (transmit_complete_ticks - transmit_start_ticks) / 16U;
      korora_debug::log("BLE_COMMAND_TX transfer=%u mtu=%u fragments=%u "
                        "receive_to_queue_us=%llu queue_to_start_us=%llu "
                        "notify_us=%llu good=%u\r\n",
                        static_cast<unsigned int>(outbound.transfer),
                        static_cast<unsigned int>(outbound.mtu),
                        static_cast<unsigned int>(count),
                        static_cast<unsigned long long>(receive_to_queue_us),
                        static_cast<unsigned long long>(queue_to_start_us),
                        static_cast<unsigned long long>(transmit_us),
                        good ? 1U : 0U);
      const bool emit_timing_record =
          outbound.mtu >= 64U || atomic_inc(&low_mtu_timing_records) < 4;
      if (emit_timing_record) {
        korora_experiment::record_transport_timing(
            outbound.transfer, outbound.command_id, outbound.operation,
            outbound.command_receive_ticks, outbound.queued_ticks,
            transmit_start_ticks, transmit_complete_ticks,
            static_cast<std::uint16_t>(count), outbound.mtu);
      }
    }
    bt_conn_unref(connection);
  }
}

K_THREAD_DEFINE(tx_thread_id, 4096, tx_thread, nullptr, nullptr, nullptr, 6, 0,
                0);

ssize_t host_write(bt_conn *, const bt_gatt_attr *, const void *buffer,
                   std::uint16_t length, std::uint16_t offset, std::uint8_t) {
  if (offset != 0U || buffer == nullptr) {
    return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
  }
  fairy::transport::FrameView frame;
  if (fairy::transport::decode_frame(static_cast<const std::uint8_t *>(buffer),
                                     length, frame) !=
      fairy::transport::DecodeResult::ok) {
    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
  }
  if (frame.header.source != fairy::config::adelie_address ||
      frame.header.channel != fairy::transport::Channel::adelie) {
    return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
  }
  fairy::transport::MessageView message;
  const auto result = host_reassembler.accept(frame, message);
  if (result == fairy::transport::DecodeResult::ok) {
    const std::uint64_t receive_ticks = korora_time::now();
    remember_command_receive(message.transfer_id, receive_ticks);
    korora_control::receive_from_adelie(message, receive_ticks);
  } else if (result != fairy::transport::DecodeResult::incomplete) {
    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
  }
  return length;
}

void host_subscription(const bt_gatt_attr *, std::uint16_t value) {
  atomic_set(&host_notifications, value == BT_GATT_CCC_NOTIFY ? 1 : 0);
  if (value == BT_GATT_CCC_NOTIFY) {
    atomic_inc(&adelie_connection_generation);
    korora_experiment::adelie_connected();
  } else {
    atomic_inc(&adelie_connection_generation);
    k_msgq_purge(&adelie_control_outbound_queue);
    k_msgq_purge(&adelie_diagnostic_outbound_queue);
    k_msgq_purge(&adelie_resume_outbound_queue);
    k_msgq_purge(&adelie_outbound_queue);
    clear_command_traces();
    korora_control::adelie_disconnected();
  }
}

std::uint8_t galapagos_notify(bt_conn *, bt_gatt_subscribe_params *params,
                              const void *data, std::uint16_t length) {
  if (data == nullptr) {
    params->value_handle = 0U;
    return BT_GATT_ITER_STOP;
  }
  fairy::transport::FrameView frame;
  if (fairy::transport::decode_frame(static_cast<const std::uint8_t *>(data),
                                     length, frame) !=
          fairy::transport::DecodeResult::ok ||
      frame.header.source != fairy::config::galapagos_address ||
      frame.header.destination != fairy::config::korora_address) {
    return BT_GATT_ITER_CONTINUE;
  }
  fairy::transport::MessageView message;
  const auto result = galapagos_reassembler.accept(frame, message);
  if (result == fairy::transport::DecodeResult::ok) {
    korora_galapagos::receive(message);
  }
  return BT_GATT_ITER_CONTINUE;
}

int subscribe_galapagos(bt_conn *connection, std::uint16_t ccc_handle) {
  std::memset(&client.subscribe, 0, sizeof(client.subscribe));
  client.subscribe.notify = galapagos_notify;
  client.subscribe.value = BT_GATT_CCC_NOTIFY;
  client.subscribe.value_handle = client.tx_handle;
  client.subscribe.ccc_handle = ccc_handle;
  const int error = bt_gatt_subscribe(connection, &client.subscribe);
  if (error == 0 || error == -EALREADY) {
    atomic_inc(&galapagos_connection_generation);
    atomic_set(&galapagos_active, 1);
    korora_galapagos::connected();
    return 0;
  }
  return error;
}

std::uint8_t discovery_callback(bt_conn *connection,
                                const bt_gatt_attr *attribute,
                                bt_gatt_discover_params *params) {
  if (attribute == nullptr) {
    std::memset(params, 0, sizeof(*params));
    return BT_GATT_ITER_STOP;
  }
  switch (client.stage) {
  case DiscoveryStage::service: {
    const auto *service =
        static_cast<const bt_gatt_service_val *>(attribute->user_data);
    client.service_end = service->end_handle;
    client.stage = DiscoveryStage::rx;
    params->uuid = &galapagos_rx_uuid.uuid;
    params->start_handle = attribute->handle + 1U;
    params->end_handle = client.service_end;
    params->type = BT_GATT_DISCOVER_CHARACTERISTIC;
    (void)bt_gatt_discover(connection, params);
    return BT_GATT_ITER_STOP;
  }
  case DiscoveryStage::rx: {
    const auto *characteristic =
        static_cast<const bt_gatt_chrc *>(attribute->user_data);
    client.rx_handle = characteristic->value_handle;
    client.stage = DiscoveryStage::tx;
    params->uuid = &galapagos_tx_uuid.uuid;
    params->start_handle = characteristic->value_handle + 1U;
    params->end_handle = client.service_end;
    params->type = BT_GATT_DISCOVER_CHARACTERISTIC;
    (void)bt_gatt_discover(connection, params);
    return BT_GATT_ITER_STOP;
  }
  case DiscoveryStage::tx: {
    const auto *characteristic =
        static_cast<const bt_gatt_chrc *>(attribute->user_data);
    client.tx_handle = characteristic->value_handle;
    client.stage = DiscoveryStage::ccc;
    params->uuid = BT_UUID_GATT_CCC;
    params->start_handle = characteristic->value_handle + 1U;
    params->end_handle = client.service_end;
    params->type = BT_GATT_DISCOVER_DESCRIPTOR;
    (void)bt_gatt_discover(connection, params);
    return BT_GATT_ITER_STOP;
  }
  case DiscoveryStage::ccc:
    client.stage = DiscoveryStage::none;
    std::memset(params, 0, sizeof(*params));
    (void)subscribe_galapagos(connection, attribute->handle);
    return BT_GATT_ITER_STOP;
  default:
    return BT_GATT_ITER_STOP;
  }
}

int start_discovery(bt_conn *connection) {
  std::memset(&client.discover, 0, sizeof(client.discover));
  client.stage = DiscoveryStage::service;
  client.discover.uuid = &galapagos_service_uuid.uuid;
  client.discover.func = discovery_callback;
  client.discover.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
  client.discover.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
  client.discover.type = BT_GATT_DISCOVER_PRIMARY;
  return bt_gatt_discover(connection, &client.discover);
}

void mtu_complete(bt_conn *connection, std::uint8_t,
                  bt_gatt_exchange_params *) {
  (void)start_discovery(connection);
}

bool name_match(bt_data *data, void *user) {
  bool *match = static_cast<bool *>(user);
  if (data->type != BT_DATA_NAME_COMPLETE &&
      data->type != BT_DATA_NAME_SHORTENED) {
    return true;
  }
  constexpr char name[] = "galapagos";
  *match = data->data_len == sizeof(name) - 1U &&
           std::memcmp(data->data, name, sizeof(name) - 1U) == 0;
  return false;
}

void scan_result(const bt_addr_le_t *address, std::int8_t rssi,
                 std::uint8_t type, net_buf_simple *data) {
  if ((type != BT_GAP_ADV_TYPE_ADV_IND &&
       type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) ||
      atomic_get(&galapagos_active) != 0 ||
      atomic_get(&galapagos_connecting) != 0) {
    return;
  }
  bool matches = false;
  bt_data_parse(data, name_match, &matches);
  if (!matches || !atomic_cas(&galapagos_connecting, 0, 1)) {
    return;
  }
  atomic_set(&last_galapagos_rssi, rssi);
  (void)bt_le_scan_stop();
  bt_conn *connection = nullptr;
  const int error =
      bt_conn_le_create(address, BT_CONN_LE_CREATE_CONN,
                        BT_LE_CONN_PARAM(8, 8, 0, 200), &connection);
  if (connection != nullptr) {
    bt_conn_unref(connection);
  }
  if (error != 0) {
    atomic_clear(&galapagos_connecting);
  }
}

int start_scan() {
  return bt_le_scan_start(&galapagos_scan_parameters, scan_result);
}

int start_advertising() {
  const bt_data advertising[] = {
      BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
      BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
              sizeof(CONFIG_BT_DEVICE_NAME) - 1),
  };
  const bt_data scan_response[] = {
      BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_ADELIE_SERVICE_VAL),
  };
  return bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, advertising,
                         ARRAY_SIZE(advertising), scan_response,
                         ARRAY_SIZE(scan_response));
}

void restart_advertising_if_host_absent() {
  k_mutex_lock(&connection_mutex, K_FOREVER);
  const bool host_absent = host_connection == nullptr;
  k_mutex_unlock(&connection_mutex);
  if (!host_absent) {
    return;
  }
  const int error = start_advertising();
  if (error != 0 && error != -EALREADY) {
    korora_debug::log("BLE_ADV_RESTART_ERROR status=%d\r\n", error);
  }
}

void recycle_work_handler(k_work *) {
  restart_advertising_if_host_absent();
  if (atomic_get(&galapagos_active) == 0 &&
      atomic_get(&galapagos_connecting) == 0) {
    const int error = start_scan();
    if (error != 0 && error != -EALREADY) {
      korora_debug::log("BLE_SCAN_RESTART_ERROR status=%d\r\n", error);
    }
  }
}

K_WORK_DEFINE(recycle_work, recycle_work_handler);

void optimize_link(bt_conn *connection, bool host_link) {
  if (host_link) {
    const int parameter_error =
        bt_conn_le_param_update(connection, BT_LE_CONN_PARAM(6, 12, 0, 200));
    if (parameter_error != 0 && parameter_error != -EALREADY &&
        parameter_error != -EBUSY) {
      korora_debug::log("BLE_PARAM_UPDATE_ERROR status=%d\r\n",
                        parameter_error);
    }
  }

  const int phy_error =
      bt_conn_le_phy_update(connection, BT_CONN_LE_PHY_PARAM_2M);
  if (phy_error != 0 && phy_error != -EALREADY && phy_error != -EBUSY) {
    korora_debug::log("BLE_PHY_UPDATE_ERROR status=%d\r\n", phy_error);
  }
  const int length_error =
      bt_conn_le_data_len_update(connection, BT_LE_DATA_LEN_PARAM_MAX);
  if (length_error != 0 && length_error != -EALREADY &&
      length_error != -EBUSY) {
    korora_debug::log("BLE_DATA_LEN_UPDATE_ERROR status=%d\r\n", length_error);
  }
}

void connected_callback(bt_conn *connection, std::uint8_t error) {
  if (error != 0U) {
    atomic_clear(&galapagos_connecting);
    (void)start_scan();
    return;
  }
  bt_conn_info info{};
  if (bt_conn_get_info(connection, &info) != 0) {
    return;
  }
  const bool host_link = info.role == BT_CONN_ROLE_PERIPHERAL;
  k_mutex_lock(&connection_mutex, K_FOREVER);
  if (host_link) {
    if (host_connection != nullptr) {
      bt_conn_unref(host_connection);
    }
    host_connection = bt_conn_ref(connection);
    korora_debug::log("BLE_HOST_CONNECTED mtu=%u interval_us=%u latency=%u "
                      "timeout_ms=%u\r\n",
                      static_cast<unsigned int>(bt_gatt_get_mtu(connection)),
                      static_cast<unsigned int>(info.le.interval) * 1250U,
                      static_cast<unsigned int>(info.le.latency),
                      static_cast<unsigned int>(info.le.timeout) * 10U);
  } else {
    if (galapagos_connection != nullptr) {
      bt_conn_unref(galapagos_connection);
    }
    galapagos_connection = bt_conn_ref(connection);
    atomic_clear(&galapagos_connecting);
    std::memset(&client.exchange, 0, sizeof(client.exchange));
    client.exchange.func = mtu_complete;
    const int mtu_error = bt_gatt_exchange_mtu(connection, &client.exchange);
    if (mtu_error == -EALREADY) {
      (void)start_discovery(connection);
    }
  }
  k_mutex_unlock(&connection_mutex);
  optimize_link(connection, host_link);
}

void parameters_updated(bt_conn *connection, std::uint16_t interval,
                        std::uint16_t latency, std::uint16_t timeout) {
  bool is_host = false;
  k_mutex_lock(&connection_mutex, K_FOREVER);
  is_host = connection == host_connection;
  k_mutex_unlock(&connection_mutex);
  if (is_host) {
    korora_debug::log(
        "BLE_HOST_PARAMS interval_us=%u latency=%u timeout_ms=%u mtu=%u\r\n",
        static_cast<unsigned int>(interval) * 1250U,
        static_cast<unsigned int>(latency),
        static_cast<unsigned int>(timeout) * 10U,
        static_cast<unsigned int>(bt_gatt_get_mtu(connection)));
  }
}

void disconnected_callback(bt_conn *connection, std::uint8_t) {
  bool was_host = false;
  bool was_galapagos = false;
  k_mutex_lock(&connection_mutex, K_FOREVER);
  if (host_connection == connection) {
    bt_conn_unref(host_connection);
    host_connection = nullptr;
    was_host = true;
  }
  if (galapagos_connection == connection) {
    bt_conn_unref(galapagos_connection);
    galapagos_connection = nullptr;
    was_galapagos = true;
  }
  k_mutex_unlock(&connection_mutex);
  if (was_host) {
    atomic_clear(&host_notifications);
    atomic_inc(&adelie_connection_generation);
    k_msgq_purge(&adelie_control_outbound_queue);
    k_msgq_purge(&adelie_diagnostic_outbound_queue);
    k_msgq_purge(&adelie_resume_outbound_queue);
    k_msgq_purge(&adelie_outbound_queue);
    reset_notification_slots();
    clear_command_traces();
    korora_control::adelie_disconnected();
  }
  if (was_galapagos) {
    atomic_clear(&galapagos_active);
    atomic_clear(&galapagos_connecting);
    atomic_inc(&galapagos_connection_generation);
    k_msgq_purge(&galapagos_outbound_queue);
    korora_galapagos::disconnected();
    korora_control::dependency_disconnected("Galapagos disconnected");
  }
}

void recycled_callback() {
  // The connection slot is available now. Bluetooth procedures are started
  // from the system work queue to avoid doing them inside the recycled
  // callback itself.
  (void)k_work_submit(&recycle_work);
}

bt_conn_cb connection_callbacks{};

bool controller_anchor_event(net_buf_simple *buffer) {
  if (buffer->len < 1U) {
    return false;
  }
  const std::uint8_t subevent = net_buf_simple_pull_u8(buffer);
  if (!fairy_sdc_is_anchor_report(subevent)) {
    return false;
  }
  fairy_sdc_anchor_report_t event{};
  if (!fairy_sdc_decode_anchor_report(buffer->data, buffer->len, &event)) {
    return true;
  }
  bt_conn *connection = bt_hci_conn_lookup_handle(event.connection_handle);
  if (connection == nullptr) {
    return true;
  }
  bool matches = false;
  k_mutex_lock(&connection_mutex, K_FOREVER);
  matches = connection == galapagos_connection;
  k_mutex_unlock(&connection_mutex);
  bt_conn_unref(connection);
  if (matches) {
    korora_galapagos::central_anchor(event.event_counter,
                                     event.anchor_point_us * 16ULL);
  }
  return true;
}

bool enqueue(Endpoint endpoint, std::uint8_t source, std::uint8_t destination,
             fairy::transport::Channel channel, const std::uint8_t *payload,
             std::size_t payload_length, std::uint16_t transfer,
             std::uint8_t flags, std::uint64_t command_receive_ticks,
             bool diagnostic_priority) {
  if (payload_length > fairy::transport::max_message_size ||
      (payload_length != 0U && payload == nullptr)) {
    return false;
  }
  Outbound outbound;
  outbound.endpoint = endpoint;
  outbound.source = source;
  outbound.destination = destination;
  outbound.channel = channel;
  outbound.flags = flags;
  outbound.transfer = transfer == 0U ? allocate_transfer() : transfer;
  outbound.length = static_cast<std::uint16_t>(payload_length);
  outbound.command_receive_ticks = command_receive_ticks;
  outbound.queued_ticks = korora_time::now();
  outbound.connection_generation = connection_generation(endpoint);
  if (payload_length != 0U) {
    std::memcpy(outbound.payload.data(), payload, payload_length);
  }
  if (channel == fairy::transport::Channel::adelie &&
      (flags & fairy::transport::response) != 0U) {
    fairy::protocol::AdelieMessageView response;
    if (fairy::protocol::decode_adelie(payload, payload_length, response)) {
      outbound.command_id = response.header.command_id;
      outbound.operation = static_cast<std::uint16_t>(response.header.opcode);
    }
  }
  if (channel == fairy::transport::Channel::adelie &&
      (flags & fairy::transport::response) != 0U) {
    const std::uint64_t remembered = take_command_receive(outbound.transfer);
    if (outbound.command_receive_ticks == 0U) {
      outbound.command_receive_ticks = remembered;
    }
  }
  k_msgq *queue =
      endpoint == Endpoint::galapagos
          ? &galapagos_outbound_queue
          : (diagnostic_priority ? &adelie_diagnostic_outbound_queue
                                 : (channel == fairy::transport::Channel::adelie
                                        ? &adelie_control_outbound_queue
                                        : &adelie_outbound_queue));
  const bool queued = k_msgq_put(queue, &outbound, K_NO_WAIT) == 0;
  if (!queued) {
    atomic_inc(endpoint == Endpoint::adelie ? &adelie_dropped
                                            : &galapagos_dropped);
  }
  return queued;
}

} // namespace

int initialize() {
  atomic_clear(&adelie_dropped);
  atomic_clear(&galapagos_dropped);
  atomic_clear(&low_mtu_timing_records);
  atomic_clear(&adelie_connection_generation);
  atomic_clear(&galapagos_connection_generation);
  int error = bt_enable(nullptr);
  if (error != 0) {
    return error;
  }
  error = korora_controller_clock::initialize();
  if (error != 0) {
    return error;
  }
  connection_callbacks.connected = connected_callback;
  connection_callbacks.disconnected = disconnected_callback;
  connection_callbacks.recycled = recycled_callback;
  connection_callbacks.le_param_updated = parameters_updated;
  bt_conn_cb_register(&connection_callbacks);
  error = bt_hci_register_vnd_evt_cb(controller_anchor_event);
  if (error != 0) {
    return error;
  }
  error = fairy_sdc_enable_anchor_reports();
  if (error != 0) {
    return error;
  }
  error = start_advertising();
  if (error != 0) {
    return error;
  }
  return start_scan();
}

bool send_to_adelie(std::uint8_t source, fairy::transport::Channel channel,
                    const std::uint8_t *payload, std::size_t payload_length,
                    std::uint16_t transfer_id, std::uint8_t flags,
                    std::uint64_t command_receive_ticks,
                    bool diagnostic_priority) {
  if (!adelie_connected()) {
    return false;
  }
  return enqueue(Endpoint::adelie, source, fairy::config::adelie_address,
                 channel, payload, payload_length, transfer_id, flags,
                 command_receive_ticks, diagnostic_priority);
}

bool send_to_galapagos(fairy::transport::Channel channel,
                       const std::uint8_t *payload, std::size_t payload_length,
                       std::uint16_t transfer_id, std::uint8_t flags) {
  if (!galapagos_connected()) {
    return false;
  }
  return enqueue(Endpoint::galapagos, fairy::config::korora_address,
                 fairy::config::galapagos_address, channel, payload,
                 payload_length, transfer_id, flags, 0, false);
}

bool adelie_connected() {
  k_mutex_lock(&connection_mutex, K_FOREVER);
  const bool connected =
      host_connection != nullptr && atomic_get(&host_notifications) != 0;
  k_mutex_unlock(&connection_mutex);
  return connected;
}

bool galapagos_connected() {
  k_mutex_lock(&connection_mutex, K_FOREVER);
  const bool connected =
      galapagos_connection != nullptr && atomic_get(&galapagos_active) != 0;
  k_mutex_unlock(&connection_mutex);
  return connected;
}

std::int8_t galapagos_rssi() {
  return static_cast<std::int8_t>(atomic_get(&last_galapagos_rssi));
}

std::uint32_t dropped_to_adelie() {
  return static_cast<std::uint32_t>(atomic_get(&adelie_dropped));
}

std::uint32_t dropped_to_galapagos() {
  return static_cast<std::uint32_t>(atomic_get(&galapagos_dropped));
}

} // namespace korora_ble
