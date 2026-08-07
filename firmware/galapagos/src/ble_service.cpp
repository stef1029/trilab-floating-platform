#include "ble_service.hpp"

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

#include "application.hpp"
#include "debug_log.hpp"
#include "fairy_shared/sdc_anchor_report.h"
#include "fairy_shared/system_config.hpp"
#include "fairy_shared/transport.hpp"
#include "record_stream.hpp"

namespace galapagos_ble {
namespace {

#define BT_UUID_GALAPAGOS_SERVICE_VAL                                          \
  BT_UUID_128_ENCODE(0xA88278D0, 0x7009, 0x4BEE, 0xA6F8, 0xE1DC3FF02B92)
#define BT_UUID_GALAPAGOS_RX_VAL                                               \
  BT_UUID_128_ENCODE(0xA88278D1, 0x7009, 0x4BEE, 0xA6F8, 0xE1DC3FF02B92)
#define BT_UUID_GALAPAGOS_TX_VAL                                               \
  BT_UUID_128_ENCODE(0xA88278D2, 0x7009, 0x4BEE, 0xA6F8, 0xE1DC3FF02B92)

const bt_uuid_128 service_uuid =
    BT_UUID_INIT_128(BT_UUID_GALAPAGOS_SERVICE_VAL);
const bt_uuid_128 rx_uuid = BT_UUID_INIT_128(BT_UUID_GALAPAGOS_RX_VAL);
const bt_uuid_128 tx_uuid = BT_UUID_INIT_128(BT_UUID_GALAPAGOS_TX_VAL);

fairy::transport::Reassembler reassembler;
bt_conn *active_connection;
atomic_t notifications_enabled;
atomic_t link_connected;
atomic_t advertising_wanted;
std::uint16_t report_every_events = 100;
std::uint16_t last_report_event;
bool have_last_report;
const bt_gatt_attr *notify_attribute;

constexpr std::int32_t advertising_restart_delay_ms = 20;
constexpr std::int32_t advertising_retry_delay_ms = 200;
const bt_le_conn_param reconnect_parameters{8U, 8U, 0U, 100U};

void advertising_work_handler(k_work *work);
K_WORK_DELAYABLE_DEFINE(advertising_work, advertising_work_handler);

ssize_t receive_write(bt_conn *, const bt_gatt_attr *, const void *buffer,
                      std::uint16_t length, std::uint16_t offset,
                      std::uint8_t) {
  if (offset != 0U || buffer == nullptr) {
    return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
  }
  fairy::transport::FrameView frame;
  if (fairy::transport::decode_frame(static_cast<const std::uint8_t *>(buffer),
                                     length, frame) !=
      fairy::transport::DecodeResult::ok) {
    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
  }
  if (frame.header.source != fairy::config::korora_address ||
      frame.header.destination != fairy::config::galapagos_address ||
      frame.header.channel != fairy::transport::Channel::adelie) {
    return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
  }

  fairy::transport::MessageView message;
  const auto result = reassembler.accept(frame, message);
  if (result == fairy::transport::DecodeResult::incomplete) {
    return length;
  }
  if (result != fairy::transport::DecodeResult::ok) {
    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
  }

  std::uint8_t response[fairy::transport::max_message_size]{};
  const std::size_t response_length = galapagos_application::handle_command(
      message.payload, message.payload_length, response, sizeof(response));
  if (response_length != 0U) {
    (void)galapagos_stream::publish_application(
        fairy::transport::Channel::adelie, fairy::config::korora_address,
        message.transfer_id, fairy::transport::response, response,
        response_length);
  }
  return length;
}

void subscription_changed(const bt_gatt_attr *, std::uint16_t value) {
  const bool enabled = value == BT_GATT_CCC_NOTIFY;
  atomic_set(&notifications_enabled, enabled ? 1 : 0);
  if (enabled && active_connection != nullptr) {
    galapagos_stream::set_connection(active_connection, notify_attribute);
    galapagos_application::connected();
  } else {
    galapagos_stream::clear_connection();
    galapagos_application::disconnected();
  }
}

BT_GATT_SERVICE_DEFINE(
    galapagos_service, BT_GATT_PRIMARY_SERVICE(&service_uuid),
    BT_GATT_CHARACTERISTIC(&rx_uuid.uuid,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, nullptr, receive_write, nullptr),
    BT_GATT_CHARACTERISTIC(&tx_uuid.uuid, BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE, nullptr, nullptr, nullptr),
    BT_GATT_CCC(subscription_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

int start_advertising() {
  const bt_data advertising[] = {
      BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
      BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
              sizeof(CONFIG_BT_DEVICE_NAME) - 1),
  };
  const bt_data scan_response[] = {
      BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_GALAPAGOS_SERVICE_VAL),
  };
  return bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, advertising,
                         ARRAY_SIZE(advertising), scan_response,
                         ARRAY_SIZE(scan_response));
}

void request_advertising(k_timeout_t delay) {
  atomic_set(&advertising_wanted, 1);
  (void)k_work_reschedule(&advertising_work, delay);
}

void advertising_work_handler(k_work *) {
  if (atomic_get(&advertising_wanted) == 0 ||
      atomic_get(&link_connected) != 0) {
    return;
  }

  const int error = start_advertising();
  if (error == 0 || error == -EALREADY) {
    atomic_clear(&advertising_wanted);
    galapagos_debug::log("BLE_ADV status=started\r\n");
    return;
  }

  galapagos_debug::log("BLE_ADV status=retry error=%d delay_ms=%d\r\n", error,
                       advertising_retry_delay_ms);
  (void)k_work_reschedule(&advertising_work,
                          K_MSEC(advertising_retry_delay_ms));
}

void connected(bt_conn *connection, std::uint8_t error) {
  if (error != 0U) {
    atomic_clear(&link_connected);
    galapagos_debug::log("BLE_CONNECT status=failed error=%u\r\n", error);
    request_advertising(K_MSEC(advertising_restart_delay_ms));
    return;
  }

  atomic_set(&link_connected, 1);
  atomic_clear(&advertising_wanted);
  (void)k_work_cancel_delayable(&advertising_work);

  if (active_connection != nullptr) {
    bt_conn_unref(active_connection);
  }
  active_connection = bt_conn_ref(connection);
  reassembler.reset();
  have_last_report = false;
  const int parameter_error =
      bt_conn_le_param_update(connection, &reconnect_parameters);
  if (parameter_error != 0 && parameter_error != -EALREADY) {
    galapagos_debug::log("BLE_CONNECT parameter_update_error=%d\r\n",
                         parameter_error);
  }
  galapagos_debug::log("BLE_CONNECT status=connected\r\n");
}

void disconnected(bt_conn *, std::uint8_t reason) {
  atomic_clear(&link_connected);
  atomic_clear(&notifications_enabled);
  galapagos_stream::clear_connection();
  reassembler.reset();
  if (active_connection != nullptr) {
    bt_conn_unref(active_connection);
    active_connection = nullptr;
  }
  galapagos_application::disconnected();
  galapagos_debug::log(
      "BLE_CONNECT status=disconnected reason=0x%02x restart_ms=%d\r\n", reason,
      advertising_restart_delay_ms);
  request_advertising(K_MSEC(advertising_restart_delay_ms));
}

void parameters_updated(bt_conn *, std::uint16_t interval, std::uint16_t,
                        std::uint16_t) {
  const std::uint32_t interval_us = interval * 1250U;
  report_every_events = static_cast<std::uint16_t>(std::max<std::uint32_t>(
      1U, (1'000'000U + interval_us / 2U) / interval_us));
}

bt_conn_cb callbacks{};

bool anchor_event(net_buf_simple *buffer) {
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
  if (atomic_get(&notifications_enabled) == 0) {
    return true;
  }
  if (!have_last_report ||
      static_cast<std::uint16_t>(event.event_counter - last_report_event) >=
          report_every_events) {
    galapagos_application::anchor_observation(event.event_counter,
                                              event.anchor_point_us);
    last_report_event = event.event_counter;
    have_last_report = true;
  }
  return true;
}

} // namespace

int initialize() {
  notify_attribute = &galapagos_service.attrs[4];
  atomic_clear(&notifications_enabled);
  atomic_clear(&link_connected);
  atomic_clear(&advertising_wanted);
  active_connection = nullptr;
  reassembler.reset();

  int error = bt_enable(nullptr);
  if (error != 0) {
    return error;
  }
  callbacks.connected = connected;
  callbacks.disconnected = disconnected;
  callbacks.le_param_updated = parameters_updated;
  bt_conn_cb_register(&callbacks);
  error = bt_hci_register_vnd_evt_cb(anchor_event);
  if (error != 0) {
    return error;
  }
  error = fairy_sdc_enable_anchor_reports();
  if (error != 0) {
    return error;
  }

  request_advertising(K_NO_WAIT);
  return 0;
}

} // namespace galapagos_ble
