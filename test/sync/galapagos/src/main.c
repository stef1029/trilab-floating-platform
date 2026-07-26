/*
 * galapagos - nRF54L15 BLE anchor and hardware GPIO-event reporter
 *
 * nRF Connect SDK 3.4.0
 * Board target: nrf54l15dk/nrf54l15/cpuapp
 *
 * Timing paths
 * ------------
 *
 * 1. BLE synchronization anchors
 *    The SoftDevice Controller reports connection-event anchor timestamps.
 *    Galapagos publishes selected anchors to Korora at approximately 1 Hz.
 *
 * 2. External GPIO events
 *    P1.11 rising edge -> GPIOTE event -> DPPI/GPPI -> GRTC CAPTURE task.
 *    The GPIOTE ISR only reads the already captured GRTC CC register and
 *    queues the record. ISR execution latency is therefore not part of the
 *    timestamp.
 *
 * Timestamp domains
 * -----------------
 *
 * GRTC SYSCOUNTER runs at 1 MHz. Captured microseconds are multiplied by 16
 * so both GPIO-event timestamps and Bluetooth anchor timestamps use the same
 * nominal 16 MHz controller-tick domain expected by Korora.
 *
 * The 40-byte frame is compatible with the existing Fairy/Galapagos frame:
 *
 *   offset  0: magic
 *   offset  1: version
 *   offset  2: status flags
 *   offset  3: frame length
 *   offset  4: record type
 *   offset  5: pending record count
 *   offset  6: record flags
 *   offset  8: captured local timestamp, 16 MHz nominal ticks
 *   offset 16: frame-build timestamp, 16 MHz nominal ticks
 *   offset 24: dropped/capture-loss count
 *   offset 28: notification/transport-error count
 *   offset 32: correlation ID
 *   offset 36: CRC-16/CCITT
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <bluetooth/hci_vs_sdc.h>
#include <gpiote_nrfx.h>
#include <hal/nrf_gpio.h>
#include <helpers/nrfx_gppi.h>
#include <nrfx_gpiote.h>
#include <nrfx_grtc.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#define NODE_NAME "galapagos"

/* -------------------------------------------------------------------------- */
/* BLE service                                                                */
/* -------------------------------------------------------------------------- */

#define BT_UUID_KORORA_SYNC_SERVICE_VAL                                        \
  BT_UUID_128_ENCODE(0xA88278D0, 0x7009, 0x4BEE, 0xA6F8, 0xE1DC3FF02B92)
#define BT_UUID_KORORA_SYNC_SERVICE                                            \
  BT_UUID_DECLARE_128(BT_UUID_KORORA_SYNC_SERVICE_VAL)

#define BT_UUID_KORORA_TTL_CONTROL_VAL                                         \
  BT_UUID_128_ENCODE(0xA88278D1, 0x7009, 0x4BEE, 0xA6F8, 0xE1DC3FF02B92)
#define BT_UUID_KORORA_TTL_CONTROL                                             \
  BT_UUID_DECLARE_128(BT_UUID_KORORA_TTL_CONTROL_VAL)

#define BT_UUID_KORORA_ANCHOR_REPORT_VAL                                       \
  BT_UUID_128_ENCODE(0xA88278D2, 0x7009, 0x4BEE, 0xA6F8, 0xE1DC3FF02B92)
#define BT_UUID_KORORA_ANCHOR_REPORT                                           \
  BT_UUID_DECLARE_128(BT_UUID_KORORA_ANCHOR_REPORT_VAL)

/* Korora to Galapagos scheduled TTL command, 20 bytes, little endian. */
#define TTL_COMMAND_MAGIC 0x544CU
#define TTL_COMMAND_VERSION 1U
#define TTL_COMMAND_OPCODE_SCHEDULE 1U
#define TTL_COMMAND_SIZE 20U
#define TTL_OFFSET_MAGIC 0U
#define TTL_OFFSET_VERSION 2U
#define TTL_OFFSET_OPCODE 3U
#define TTL_OFFSET_SEQUENCE 4U
#define TTL_OFFSET_TARGET_TICKS 8U
#define TTL_OFFSET_PULSE_WIDTH_US 16U
#define TTL_MINIMUM_LEAD_US 50000ULL
#define TTL_MAXIMUM_WIDTH_US 2000000U
/* -------------------------------------------------------------------------- */
/* Timing and report cadence                                                  */
/* -------------------------------------------------------------------------- */

#define CONTROLLER_TICKS_PER_US 16ULL

#define REPORT_RATE_HZ 1U
#define REPORT_PERIOD_US (1000000U / REPORT_RATE_HZ)
#define DEFAULT_CONNECTION_INTERVAL_US 10000U

/* -------------------------------------------------------------------------- */
/* Shared 40-byte wire frame                                                  */
/* -------------------------------------------------------------------------- */

#define SYNC_FRAME_MAGIC 0xA5U
#define SYNC_FRAME_VERSION 2U
#define SYNC_FRAME_SIZE 40U

#define FRAME_OFFSET_MAGIC 0U
#define FRAME_OFFSET_VERSION 1U
#define FRAME_OFFSET_STATUS_FLAGS 2U
#define FRAME_OFFSET_LENGTH 3U
#define FRAME_OFFSET_RECORD_TYPE 4U
#define FRAME_OFFSET_PENDING_COUNT 5U
#define FRAME_OFFSET_RECORD_FLAGS 6U
#define FRAME_OFFSET_CAPTURE_TICKS 8U
#define FRAME_OFFSET_SNAPSHOT_TICKS 16U
#define FRAME_OFFSET_CAPTURE_LOSS_COUNT 24U
#define FRAME_OFFSET_TRANSPORT_ERROR_COUNT 28U
#define FRAME_OFFSET_CORRELATION_ID 32U
#define FRAME_OFFSET_CRC 36U

#define SYNC_STATUS_RECORD_VALID BIT(0)
#define SYNC_STATUS_FIRST_AFTER_RESET BIT(1)
#define SYNC_STATUS_CAPTURE_LOSS_LATCHED BIT(2)
#define SYNC_STATUS_TRANSPORT_ERROR_LATCHED BIT(3)
#define SYNC_STATUS_CLOCK_FAULT BIT(4)

#define SYNC_RECORD_SYNC 1U
#define SYNC_RECORD_EVENT 2U
#define SYNC_RECORD_TTL_GENERATED 4U

#define SYNC_RECORD_FLAG_NONE 0U

BUILD_ASSERT(SYNC_FRAME_SIZE == (FRAME_OFFSET_CRC + 4U),
             "Frame offsets do not fit the declared frame size");

/* -------------------------------------------------------------------------- */
/* External event input                                                       */
/* -------------------------------------------------------------------------- */

#define USER_NODE DT_PATH(zephyr_user)

BUILD_ASSERT(DT_NODE_HAS_PROP(USER_NODE, event_gpios),
             "Overlay must define zephyr,user event-gpios");
BUILD_ASSERT(DT_NODE_HAS_PROP(USER_NODE, ttl_output_gpios),
             "Overlay must define zephyr,user ttl-output-gpios");

#define EVENT_INPUT_PIN NRF_DT_GPIOS_TO_PSEL(USER_NODE, event_gpios)
#define EVENT_GPIOTE_NODE NRF_DT_GPIOTE_NODE(USER_NODE, event_gpios)
#define TTL_OUTPUT_PIN NRF_DT_GPIOS_TO_PSEL(USER_NODE, ttl_output_gpios)
#define TTL_OUTPUT_GPIOTE_NODE NRF_DT_GPIOTE_NODE(USER_NODE, ttl_output_gpios)

BUILD_ASSERT(DT_SAME_NODE(EVENT_GPIOTE_NODE, TTL_OUTPUT_GPIOTE_NODE),
             "Event input and TTL output must use the same GPIOTE instance");

static nrfx_gpiote_t *const event_gpiote =
    &GPIOTE_NRFX_INST_BY_NODE(EVENT_GPIOTE_NODE);

static uint8_t event_gpiote_channel;
static uint8_t event_grtc_channel;
static nrfx_gppi_handle_t event_capture_connection;

static uint8_t ttl_output_gpiote_channel;
static uint8_t ttl_set_grtc_channel;
static uint8_t ttl_clear_grtc_channel;
static nrfx_gppi_handle_t ttl_set_connection;
static nrfx_gppi_handle_t ttl_clear_connection;
static atomic_t ttl_pulse_armed = ATOMIC_INIT(0);

struct ttl_pulse_state {
  uint32_t sequence;
  uint64_t target_us;
  uint32_t pulse_width_us;
};

static struct ttl_pulse_state ttl_pulse;

/*
 * VCOM debug for the scheduled TTL compare. The GRTC callback may execute in
 * interrupt context, so it records the values and defers vcom_log to k_work.
 */
struct ttl_sent_debug_state {
  uint32_t sequence;
  uint64_t target_us;
  uint64_t callback_us;
  uint32_t pulse_width_us;
  uint8_t pin_level;
};

static struct ttl_sent_debug_state ttl_sent_debug;

#include <stdarg.h>

K_MUTEX_DEFINE(vcom_log_mutex);

static void vcom_log(const char *format, ...) {
  char line[192];
  va_list arguments;

  va_start(arguments, format);
  const int length = vsnprintk(line, sizeof(line), format, arguments);
  va_end(arguments);

  if ((length <= 0) || (length >= sizeof(line))) {
    return;
  }

  k_mutex_lock(&vcom_log_mutex, K_FOREVER);
  printk("%s", line);
  k_mutex_unlock(&vcom_log_mutex);
}

static void ttl_sent_log_work_handler(struct k_work *work) {
  ARG_UNUSED(work);

  const struct ttl_sent_debug_state snapshot = ttl_sent_debug;
  const int64_t callback_delay_us =
      (int64_t)snapshot.callback_us - (int64_t)snapshot.target_us;

  vcom_log("TTL_PULSE_SENT,%s,%u,%llu,%llu,%lld,%u,pin=%u\n", NODE_NAME,
           snapshot.sequence,
           (unsigned long long)(snapshot.target_us * CONTROLLER_TICKS_PER_US),
           (unsigned long long)snapshot.callback_us,
           (long long)callback_delay_us, snapshot.pulse_width_us,
           (unsigned int)snapshot.pin_level);
}

K_WORK_DEFINE(ttl_sent_log_work, ttl_sent_log_work_handler);

/* -------------------------------------------------------------------------- */
/* Outgoing records and serialized notification transport                     */
/* -------------------------------------------------------------------------- */

#define OUTGOING_QUEUE_DEPTH 16U
#define NOTIFY_RETRY_DELAY_MS 5U
#define NOTIFY_COMPLETION_WARNING_MS 2000U

struct outgoing_record {
  uint8_t record_type;
  uint8_t status_flags;
  uint8_t record_flags;
  uint64_t capture_ticks;
  uint32_t correlation_id;
};

K_MSGQ_DEFINE(outgoing_record_queue, sizeof(struct outgoing_record),
              OUTGOING_QUEUE_DEPTH, 4);
K_SEM_DEFINE(notification_complete_sem, 0, 1);

static atomic_t external_event_sequence = ATOMIC_INIT(0);
static atomic_t dropped_report_count = ATOMIC_INIT(0);
static atomic_t notify_error_count = ATOMIC_INIT(0);

/* -------------------------------------------------------------------------- */
/* Connection state                                                           */
/* -------------------------------------------------------------------------- */

static struct bt_conn *active_conn;
static uint8_t active_conn_index;
K_MUTEX_DEFINE(active_conn_mutex);

static atomic_t active_connection_present = ATOMIC_INIT(0);
static atomic_t notifications_enabled = ATOMIC_INIT(0);

static bool have_last_report_counter;
static uint16_t last_report_counter;
static uint16_t report_every_connection_events = 100U;
static bool first_report_after_reset = true;

/* -------------------------------------------------------------------------- */
/* Forward declarations                                                       */
/* -------------------------------------------------------------------------- */

static uint16_t crc16_ccitt(const uint8_t *data, size_t length);
static struct bt_conn *active_conn_ref_get(void);
static int advertising_start(void);
static int configure_external_event_capture(void);
static int configure_ttl_output(void);
static ssize_t ttl_control_write(struct bt_conn *conn,
                                 const struct bt_gatt_attr *attr,
                                 const void *buffer, uint16_t length,
                                 uint16_t offset, uint8_t flags);

static void notifications_changed(const struct bt_gatt_attr *attr,
                                  uint16_t value);
static void advertising_restart_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(advertising_restart_work,
                        advertising_restart_work_handler);

/* -------------------------------------------------------------------------- */
/* GATT service                                                               */
/* -------------------------------------------------------------------------- */

BT_GATT_SERVICE_DEFINE(
    korora_sync_service, BT_GATT_PRIMARY_SERVICE(BT_UUID_KORORA_SYNC_SERVICE),
    BT_GATT_CHARACTERISTIC(BT_UUID_KORORA_TTL_CONTROL,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, ttl_control_write, NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_KORORA_ANCHOR_REPORT, BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(notifications_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

/* -------------------------------------------------------------------------- */
/* Utilities                                                                  */
/* -------------------------------------------------------------------------- */

static uint16_t crc16_ccitt(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFFU;

  for (size_t i = 0U; i < length; ++i) {
    crc ^= (uint16_t)data[i] << 8;

    for (uint32_t bit = 0U; bit < 8U; ++bit) {
      if ((crc & 0x8000U) != 0U) {
        crc = (uint16_t)(((uint32_t)crc << 1) ^ 0x1021U);
      } else {
        crc = (uint16_t)((uint32_t)crc << 1);
      }
    }
  }

  return crc;
}

static struct bt_conn *active_conn_ref_get(void) {
  struct bt_conn *conn = NULL;

  k_mutex_lock(&active_conn_mutex, K_FOREVER);

  if (active_conn != NULL) {
    conn = bt_conn_ref(active_conn);
  }

  k_mutex_unlock(&active_conn_mutex);

  return conn;
}

static bool outgoing_record_enqueue(const struct outgoing_record *record) {
  if ((atomic_get(&active_connection_present) == 0) ||
      (atomic_get(&notifications_enabled) == 0)) {
    (void)atomic_inc(&dropped_report_count);
    return false;
  }

  if (k_msgq_put(&outgoing_record_queue, record, K_NO_WAIT) != 0) {
    (void)atomic_inc(&dropped_report_count);
    return false;
  }

  return true;
}

static void build_report_frame(uint8_t frame[SYNC_FRAME_SIZE],
                               const struct outgoing_record *record) {
  memset(frame, 0, SYNC_FRAME_SIZE);

  frame[FRAME_OFFSET_MAGIC] = SYNC_FRAME_MAGIC;
  frame[FRAME_OFFSET_VERSION] = SYNC_FRAME_VERSION;
  frame[FRAME_OFFSET_STATUS_FLAGS] =
      SYNC_STATUS_RECORD_VALID | record->status_flags;
  frame[FRAME_OFFSET_LENGTH] = SYNC_FRAME_SIZE;
  frame[FRAME_OFFSET_RECORD_TYPE] = record->record_type;

  /*
   * The current record has already been removed from the message queue, so
   * include it explicitly in the pending count.
   */
  frame[FRAME_OFFSET_PENDING_COUNT] =
      (uint8_t)MIN(k_msgq_num_used_get(&outgoing_record_queue) + 1U, UINT8_MAX);

  frame[FRAME_OFFSET_RECORD_FLAGS] = record->record_flags;

  sys_put_le64(record->capture_ticks, &frame[FRAME_OFFSET_CAPTURE_TICKS]);

  sys_put_le64(nrfx_grtc_syscounter_get() * CONTROLLER_TICKS_PER_US,
               &frame[FRAME_OFFSET_SNAPSHOT_TICKS]);

  sys_put_le32((uint32_t)atomic_get(&dropped_report_count),
               &frame[FRAME_OFFSET_CAPTURE_LOSS_COUNT]);

  sys_put_le32((uint32_t)atomic_get(&notify_error_count),
               &frame[FRAME_OFFSET_TRANSPORT_ERROR_COUNT]);

  sys_put_le32(record->correlation_id, &frame[FRAME_OFFSET_CORRELATION_ID]);

  sys_put_le16(crc16_ccitt(frame, FRAME_OFFSET_CRC), &frame[FRAME_OFFSET_CRC]);
}

/* -------------------------------------------------------------------------- */
/* Serialized notification thread                                             */
/* -------------------------------------------------------------------------- */

static void notification_complete(struct bt_conn *conn, void *user_data) {
  ARG_UNUSED(conn);
  ARG_UNUSED(user_data);

  k_sem_give(&notification_complete_sem);
}

static void notification_thread(void *argument_1, void *argument_2,
                                void *argument_3) {
  ARG_UNUSED(argument_1);
  ARG_UNUSED(argument_2);
  ARG_UNUSED(argument_3);

  struct outgoing_record record;

  while (true) {
    k_msgq_get(&outgoing_record_queue, &record, K_FOREVER);

    if (atomic_get(&notifications_enabled) == 0) {
      (void)atomic_inc(&dropped_report_count);
      continue;
    }

    struct bt_conn *conn = active_conn_ref_get();

    if (conn == NULL) {
      (void)atomic_inc(&dropped_report_count);
      continue;
    }

    uint8_t frame[SYNC_FRAME_SIZE];
    build_report_frame(frame, &record);

    struct bt_gatt_notify_params parameters = {
        .attr = &korora_sync_service.attrs[4],
        .data = frame,
        .len = sizeof(frame),
        .func = notification_complete,
        .user_data = NULL,
    };

    int error;

    while (true) {
      k_sem_reset(&notification_complete_sem);

      error = bt_gatt_notify_cb(conn, &parameters);

      if ((error == -ENOMEM) || (error == -EAGAIN)) {
        k_sleep(K_MSEC(NOTIFY_RETRY_DELAY_MS));
        continue;
      }

      break;
    }

    if (error != 0) {
      (void)atomic_inc(&notify_error_count);
      vcom_log("BLE_NOTIFY_ERROR,%s,%u,%u,%d\n", NODE_NAME, record.record_type,
               record.correlation_id, error);
      bt_conn_unref(conn);
      continue;
    }

    /*
     * The frame is stack-resident, so keep this thread blocked until the
     * Bluetooth host has finished using it. Log a warning after two seconds,
     * but continue waiting so the buffer lifetime remains valid.
     */
    const int wait_error = k_sem_take(&notification_complete_sem,
                                      K_MSEC(NOTIFY_COMPLETION_WARNING_MS));

    if (wait_error != 0) {
      (void)atomic_inc(&notify_error_count);
      vcom_log("BLE_NOTIFY_SLOW,%s,%u,%u,%d\n", NODE_NAME, record.record_type,
               record.correlation_id, wait_error);

      (void)k_sem_take(&notification_complete_sem, K_FOREVER);
    }

    bt_conn_unref(conn);
  }
}

K_THREAD_DEFINE(notification_thread_id, 2048, notification_thread, NULL, NULL,
                NULL, 7, 0, 0);

/* -------------------------------------------------------------------------- */
/* Hardware GPIO capture                                                      */
/* -------------------------------------------------------------------------- */

static void external_event_gpio_handler(nrfx_gpiote_pin_t pin,
                                        nrfx_gpiote_trigger_t trigger,
                                        void *context) {
  ARG_UNUSED(pin);
  ARG_UNUSED(trigger);
  ARG_UNUSED(context);

  /*
   * The GPIO edge has already triggered the GRTC CAPTURE task through DPPI.
   * This read retrieves the captured edge time, not the ISR execution time.
   */
  uint64_t captured_us = 0ULL;

  const int error =
      nrfx_grtc_syscounter_cc_value_read(event_grtc_channel, &captured_us);

  if (error != 0) {
    (void)atomic_inc(&dropped_report_count);
    return;
  }

  const uint32_t sequence = (uint32_t)atomic_inc(&external_event_sequence) + 1U;

  const struct outgoing_record record = {
      .record_type = SYNC_RECORD_EVENT,
      .status_flags = 0U,
      .record_flags = SYNC_RECORD_FLAG_NONE,
      .capture_ticks = captured_us * CONTROLLER_TICKS_PER_US,
      .correlation_id = sequence,
  };

  (void)outgoing_record_enqueue(&record);
}

static void gpiote_irq_wrapper(const void *argument) {
  nrfx_gpiote_irq_handler((nrfx_gpiote_t *)argument);
}

static int wait_for_grtc_ready(void) {
  /* The Zephyr system timer normally starts GRTC before main(). */
  for (uint32_t attempt = 0U; attempt < 10000U; ++attempt) {
    if (nrfx_grtc_ready_check()) {
      return 0;
    }

    k_busy_wait(10U);
  }

  return -ETIMEDOUT;
}

static int configure_external_event_capture(void) {
  int error;

  /* Keep the 1 MHz SYSCOUNTER awake for asynchronous GPIO captures. */
  nrfx_grtc_active_request_set(true);

  error = wait_for_grtc_ready();

  if (error != 0) {
    return error;
  }

  error = nrfx_grtc_channel_alloc(&event_grtc_channel);

  if (error != 0) {
    return error;
  }

  /*
   * nrfx_grtc_syscounter_capture() performs one initialization capture and,
   * importantly, marks the allocated channel as used by the SYSCOUNTER API.
   * The value captured here is intentionally ignored. Every later GPIO edge
   * overwrites the CC register through the hardware capture task.
   */
  error = nrfx_grtc_syscounter_capture(event_grtc_channel);

  if (error != 0) {
    (void)nrfx_grtc_channel_free(event_grtc_channel);
    return error;
  }

  IRQ_CONNECT(DT_IRQN(EVENT_GPIOTE_NODE), DT_IRQ(EVENT_GPIOTE_NODE, priority),
              gpiote_irq_wrapper, event_gpiote, 0);

  if (!nrfx_gpiote_init_check(event_gpiote)) {
    error = nrfx_gpiote_init(event_gpiote, DT_IRQ(EVENT_GPIOTE_NODE, priority));

    if (error != 0) {
      (void)nrfx_grtc_channel_free(event_grtc_channel);
      return error;
    }
  }

  irq_enable(DT_IRQN(EVENT_GPIOTE_NODE));

  error = nrfx_gpiote_channel_alloc(event_gpiote, &event_gpiote_channel);

  if (error != 0) {
    (void)nrfx_grtc_channel_free(event_grtc_channel);
    return error;
  }

  static const nrf_gpio_pin_pull_t pull = NRF_GPIO_PIN_PULLDOWN;

  const nrfx_gpiote_trigger_config_t trigger_config = {
      .trigger = NRFX_GPIOTE_TRIGGER_LOTOHI,
      .p_in_channel = &event_gpiote_channel,
  };

  const nrfx_gpiote_handler_config_t handler_config = {
      .handler = external_event_gpio_handler,
      .p_context = NULL,
  };

  const nrfx_gpiote_input_pin_config_t input_config = {
      .p_pull_config = &pull,
      .p_trigger_config = &trigger_config,
      .p_handler_config = &handler_config,
  };

  error =
      nrfx_gpiote_input_configure(event_gpiote, EVENT_INPUT_PIN, &input_config);

  if (error != 0) {
    (void)nrfx_gpiote_channel_free(event_gpiote, event_gpiote_channel);
    (void)nrfx_grtc_channel_free(event_grtc_channel);
    return error;
  }

  /*
   * Hardware timestamp path:
   *
   *   GPIOTE IN event -> DPPI/GPPI -> GRTC CAPTURE[event_grtc_channel]
   */
  error = nrfx_gppi_conn_alloc(
      nrfx_gpiote_in_event_address_get(event_gpiote, EVENT_INPUT_PIN),
      nrfx_grtc_capture_task_address_get(event_grtc_channel),
      &event_capture_connection);

  if (error != 0) {
    (void)nrfx_gpiote_pin_uninit(event_gpiote, EVENT_INPUT_PIN);
    (void)nrfx_gpiote_channel_free(event_gpiote, event_gpiote_channel);
    (void)nrfx_grtc_channel_free(event_grtc_channel);
    return error;
  }

  nrfx_gppi_conn_enable(event_capture_connection);

  /* true enables the GPIOTE IRQ used only to retrieve and queue the CC value.
   */
  nrfx_gpiote_trigger_enable(event_gpiote, EVENT_INPUT_PIN, true);

  vcom_log("EVENT_CAPTURE_READY,%s,pin=%u,gpiote_ch=%u,grtc_ch=%u\n", NODE_NAME,
           (unsigned int)EVENT_INPUT_PIN, (unsigned int)event_gpiote_channel,
           (unsigned int)event_grtc_channel);

  return 0;
}

/* -------------------------------------------------------------------------- */
/* Scheduled TTL output                                                       */
/* -------------------------------------------------------------------------- */

static void ttl_set_compare_handler(int32_t id, uint64_t cc_value,
                                    void *context) {
  ARG_UNUSED(context);

  if (id != (int32_t)ttl_set_grtc_channel) {
    return;
  }

  ttl_sent_debug.sequence = ttl_pulse.sequence;
  ttl_sent_debug.target_us = cc_value;
  ttl_sent_debug.callback_us = nrfx_grtc_syscounter_get();
  ttl_sent_debug.pulse_width_us = ttl_pulse.pulse_width_us;
  ttl_sent_debug.pin_level = (uint8_t)(nrf_gpio_pin_read(TTL_OUTPUT_PIN) != 0U);
  (void)k_work_submit(&ttl_sent_log_work);

  const struct outgoing_record record = {
      .record_type = SYNC_RECORD_TTL_GENERATED,
      .status_flags = 0U,
      .record_flags = SYNC_RECORD_FLAG_NONE,
      .capture_ticks = cc_value * CONTROLLER_TICKS_PER_US,
      .correlation_id = ttl_pulse.sequence,
  };

  (void)outgoing_record_enqueue(&record);
}

static void ttl_clear_compare_handler(int32_t id, uint64_t cc_value,
                                      void *context) {
  ARG_UNUSED(cc_value);
  ARG_UNUSED(context);

  if (id == (int32_t)ttl_clear_grtc_channel) {
    atomic_clear(&ttl_pulse_armed);
  }
}

static ssize_t ttl_control_write(struct bt_conn *conn,
                                 const struct bt_gatt_attr *attr,
                                 const void *buffer, uint16_t length,
                                 uint16_t offset, uint8_t flags) {
  ARG_UNUSED(conn);
  ARG_UNUSED(attr);
  ARG_UNUSED(flags);

  if ((offset != 0U) || (length != TTL_COMMAND_SIZE)) {
    return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
  }

  const uint8_t *frame = buffer;
  const uint16_t magic = sys_get_le16(&frame[TTL_OFFSET_MAGIC]);
  const uint8_t version = frame[TTL_OFFSET_VERSION];
  const uint8_t opcode = frame[TTL_OFFSET_OPCODE];
  const uint32_t sequence = sys_get_le32(&frame[TTL_OFFSET_SEQUENCE]);
  const uint64_t target_ticks = sys_get_le64(&frame[TTL_OFFSET_TARGET_TICKS]);
  const uint32_t pulse_width_us =
      sys_get_le32(&frame[TTL_OFFSET_PULSE_WIDTH_US]);

  if ((magic != TTL_COMMAND_MAGIC) || (version != TTL_COMMAND_VERSION) ||
      (opcode != TTL_COMMAND_OPCODE_SCHEDULE) ||
      ((target_ticks % CONTROLLER_TICKS_PER_US) != 0ULL) ||
      (pulse_width_us == 0U) || (pulse_width_us > TTL_MAXIMUM_WIDTH_US)) {
    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
  }

  const uint64_t target_us = target_ticks / CONTROLLER_TICKS_PER_US;
  const uint64_t now_us = nrfx_grtc_syscounter_get();

  if ((target_us <= now_us) || ((target_us - now_us) < TTL_MINIMUM_LEAD_US) ||
      ((UINT64_MAX - target_us) < pulse_width_us)) {
    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
  }

  if (!atomic_cas(&ttl_pulse_armed, 0, 1)) {
    return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
  }

  ttl_pulse.sequence = sequence;
  ttl_pulse.target_us = target_us;
  ttl_pulse.pulse_width_us = pulse_width_us;

  vcom_log("TTL_COMMAND_RECEIVED,%s,%u,%llu,%llu,%llu,%u\n", NODE_NAME,
           sequence, (unsigned long long)target_ticks,
           (unsigned long long)now_us, (unsigned long long)(target_us - now_us),
           pulse_width_us);

  /*
   * The optimized absolute compare setter requires interrupts locked.
   * The two hardware compare events drive GPIOTE SET and CLR through DPPI.
   */
  const unsigned int key = irq_lock();
  nrfx_grtc_syscounter_cc_abs_set(ttl_set_grtc_channel, target_us, true);
  nrfx_grtc_syscounter_cc_abs_set(ttl_clear_grtc_channel,
                                  target_us + pulse_width_us, true);
  irq_unlock(key);

  vcom_log("TTL_ARMED,%s,%u,%llu,%u\n", NODE_NAME, sequence,
           (unsigned long long)target_ticks, pulse_width_us);

  return length;
}

static int configure_ttl_output(void) {
#if !defined(GPIOTE_FEATURE_SET_PRESENT) || !defined(GPIOTE_FEATURE_CLR_PRESENT)
#error "Scheduled TTL output requires GPIOTE SET and CLR tasks"
#endif

  int error =
      nrfx_gpiote_channel_alloc(event_gpiote, &ttl_output_gpiote_channel);
  if (error != 0) {
    return error;
  }

  static const nrfx_gpiote_output_config_t output_config = {
      .drive = NRF_GPIO_PIN_S0S1,
      .input_connect = NRF_GPIO_PIN_INPUT_CONNECT,
      .pull = NRF_GPIO_PIN_NOPULL,
  };

  const nrfx_gpiote_task_config_t task_config = {
      .task_ch = ttl_output_gpiote_channel,
      .polarity = NRF_GPIOTE_POLARITY_TOGGLE,
      .init_val = NRF_GPIOTE_INITIAL_VALUE_LOW,
  };

  error = nrfx_gpiote_output_configure(event_gpiote, TTL_OUTPUT_PIN,
                                       &output_config, &task_config);
  if (error != 0) {
    return error;
  }

  nrfx_gpiote_out_task_enable(event_gpiote, TTL_OUTPUT_PIN);
  nrfx_gpiote_clr_task_trigger(event_gpiote, TTL_OUTPUT_PIN);

  error = nrfx_grtc_channel_alloc(&ttl_set_grtc_channel);
  if (error != 0) {
    return error;
  }

  error = nrfx_grtc_channel_alloc(&ttl_clear_grtc_channel);
  if (error != 0) {
    return error;
  }

  nrfx_grtc_channel_callback_set(ttl_set_grtc_channel, ttl_set_compare_handler,
                                 NULL);
  nrfx_grtc_channel_callback_set(ttl_clear_grtc_channel,
                                 ttl_clear_compare_handler, NULL);

  error = nrfx_gppi_conn_alloc(
      nrfx_grtc_event_compare_address_get(ttl_set_grtc_channel),
      nrfx_gpiote_set_task_address_get(event_gpiote, TTL_OUTPUT_PIN),
      &ttl_set_connection);
  if (error != 0) {
    return error;
  }

  error = nrfx_gppi_conn_alloc(
      nrfx_grtc_event_compare_address_get(ttl_clear_grtc_channel),
      nrfx_gpiote_clr_task_address_get(event_gpiote, TTL_OUTPUT_PIN),
      &ttl_clear_connection);
  if (error != 0) {
    return error;
  }

  nrfx_gppi_conn_enable(ttl_set_connection);
  nrfx_gppi_conn_enable(ttl_clear_connection);

  vcom_log(
      "TTL_OUTPUT_READY,%s,pin=%u,gpiote_ch=%u,set_grtc_ch=%u,clear_grtc_ch="
      "%u\n",
      NODE_NAME, (unsigned int)TTL_OUTPUT_PIN,
      (unsigned int)ttl_output_gpiote_channel,
      (unsigned int)ttl_set_grtc_channel, (unsigned int)ttl_clear_grtc_channel);

  return 0;
}

/* -------------------------------------------------------------------------- */
/* GATT CCC and advertising                                                   */
/* -------------------------------------------------------------------------- */

static void notifications_changed(const struct bt_gatt_attr *attr,
                                  uint16_t value) {
  ARG_UNUSED(attr);

  const bool enabled = (value == BT_GATT_CCC_NOTIFY);
  atomic_set(&notifications_enabled, enabled ? 1 : 0);

  if (!enabled) {
    k_msgq_purge(&outgoing_record_queue);
  }

  vcom_log("BLE_NOTIFY_STATE,%s,%s\n", NODE_NAME,
           enabled ? "ENABLED" : "DISABLED");
}

static const struct bt_data advertising_data[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1U),
};

static const struct bt_data scan_response_data[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_KORORA_SYNC_SERVICE_VAL),
};

static int advertising_start(void) {
  const int error = bt_le_adv_start(
      BT_LE_ADV_CONN_FAST_2, advertising_data, ARRAY_SIZE(advertising_data),
      scan_response_data, ARRAY_SIZE(scan_response_data));

  if ((error != 0) && (error != -EALREADY)) {
    vcom_log("BLE_ADV_ERROR,%s,%d\n", NODE_NAME, error);
    return error;
  }

  vcom_log("BLE_ADVERTISING,%s\n", NODE_NAME);
  return 0;
}

static void advertising_restart_work_handler(struct k_work *work) {
  ARG_UNUSED(work);

  const int error = advertising_start();

  if ((error == -ENOMEM) || (error == -EAGAIN)) {
    (void)k_work_reschedule(&advertising_restart_work, K_MSEC(250));
  }
}

/* -------------------------------------------------------------------------- */
/* Connection lifecycle                                                       */
/* -------------------------------------------------------------------------- */

static void report_cadence_update(uint32_t interval_us) {
  if (interval_us == 0U) {
    interval_us = DEFAULT_CONNECTION_INTERVAL_US;
  }

  uint32_t events = (REPORT_PERIOD_US + (interval_us / 2U)) / interval_us;

  if (events == 0U) {
    events = 1U;
  }

  if (events > UINT16_MAX) {
    events = UINT16_MAX;
  }

  report_every_connection_events = (uint16_t)events;
  have_last_report_counter = false;

  vcom_log("BLE_REPORT_CADENCE,%s,%u,%u,%u\n", NODE_NAME, interval_us,
           report_every_connection_events, REPORT_RATE_HZ);
}

static void connected(struct bt_conn *conn, uint8_t error) {
  char address[BT_ADDR_LE_STR_LEN];
  bt_addr_le_to_str(bt_conn_get_dst(conn), address, sizeof(address));

  if (error != 0U) {
    vcom_log("BLE_CONNECT_FAILED,%s,%s,0x%02x\n", NODE_NAME, address, error);
    (void)k_work_reschedule(&advertising_restart_work, K_MSEC(100));
    return;
  }

  k_mutex_lock(&active_conn_mutex, K_FOREVER);

  if (active_conn != NULL) {
    bt_conn_unref(active_conn);
  }

  active_conn = bt_conn_ref(conn);
  active_conn_index = bt_conn_index(conn);

  k_mutex_unlock(&active_conn_mutex);

  atomic_set(&active_connection_present, 1);
  atomic_clear(&notifications_enabled);

  have_last_report_counter = false;
  first_report_after_reset = true;
  k_msgq_purge(&outgoing_record_queue);

  struct bt_conn_info info;
  uint32_t interval_us = DEFAULT_CONNECTION_INTERVAL_US;

  if (bt_conn_get_info(conn, &info) == 0) {
    interval_us = info.le.interval_us;
  }

  report_cadence_update(interval_us);
  atomic_clear(&ttl_pulse_armed);
  nrfx_gpiote_clr_task_trigger(event_gpiote, TTL_OUTPUT_PIN);
  nrfx_gppi_conn_enable(ttl_set_connection);
  nrfx_gppi_conn_enable(ttl_clear_connection);

  vcom_log("BLE_CONNECTED,%s,%s,%u,%u\n", NODE_NAME, address, interval_us,
           report_every_connection_events);
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
  char address[BT_ADDR_LE_STR_LEN];
  bt_addr_le_to_str(bt_conn_get_dst(conn), address, sizeof(address));

  vcom_log("BLE_DISCONNECTED,%s,%s,0x%02x\n", NODE_NAME, address, reason);

  atomic_clear(&active_connection_present);
  atomic_clear(&notifications_enabled);

  k_mutex_lock(&active_conn_mutex, K_FOREVER);

  if (active_conn != NULL) {
    bt_conn_unref(active_conn);
    active_conn = NULL;
  }

  k_mutex_unlock(&active_conn_mutex);

  have_last_report_counter = false;
  first_report_after_reset = true;
  k_msgq_purge(&outgoing_record_queue);
  atomic_clear(&ttl_pulse_armed);
  nrfx_gppi_conn_disable(ttl_set_connection);
  nrfx_gpiote_clr_task_trigger(event_gpiote, TTL_OUTPUT_PIN);

  (void)k_work_reschedule(&advertising_restart_work, K_MSEC(100));
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval,
                             uint16_t latency, uint16_t timeout) {
  ARG_UNUSED(conn);

  const uint32_t interval_us = (uint32_t)interval * 1250U;
  report_cadence_update(interval_us);

  vcom_log("BLE_PARAMS,%s,%u,%u,%u,%u\n", NODE_NAME, interval, interval_us,
           latency, timeout);
}

static struct bt_conn_cb connection_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
    .le_param_updated = le_param_updated,
};

/* -------------------------------------------------------------------------- */
/* SoftDevice Controller anchor reports                                       */
/* -------------------------------------------------------------------------- */

static bool anchor_point_event(struct net_buf_simple *buffer) {
  if (buffer->len < 1U) {
    return false;
  }

  const uint8_t subevent_code = net_buf_simple_pull_u8(buffer);

  if (subevent_code != SDC_HCI_SUBEVENT_VS_CONN_ANCHOR_POINT_UPDATE_REPORT) {
    return false;
  }

  if (buffer->len <
      sizeof(sdc_hci_subevent_vs_conn_anchor_point_update_report_t)) {
    return true;
  }

  const sdc_hci_subevent_vs_conn_anchor_point_update_report_t *event =
      (const void *)buffer->data;

  struct bt_conn *conn = bt_hci_conn_lookup_handle(event->conn_handle);

  if (conn == NULL) {
    return true;
  }

  const uint8_t connection_index = bt_conn_index(conn);
  bt_conn_unref(conn);

  if ((atomic_get(&active_connection_present) == 0) ||
      (connection_index != active_conn_index) ||
      (atomic_get(&notifications_enabled) == 0)) {
    return true;
  }

  const bool report_due =
      !have_last_report_counter ||
      ((uint16_t)(event->event_counter - last_report_counter) >=
       report_every_connection_events);

  if (!report_due) {
    return true;
  }

  const struct outgoing_record record = {
      .record_type = SYNC_RECORD_SYNC,
      .status_flags =
          first_report_after_reset ? SYNC_STATUS_FIRST_AFTER_RESET : 0U,
      .record_flags = SYNC_RECORD_FLAG_NONE,
      .capture_ticks = event->anchor_point_us * CONTROLLER_TICKS_PER_US,
      .correlation_id = event->event_counter,
  };

  if (!outgoing_record_enqueue(&record)) {
    return true;
  }

  last_report_counter = event->event_counter;
  have_last_report_counter = true;
  first_report_after_reset = false;

  return true;
}

/* -------------------------------------------------------------------------- */
/* Main                                                                       */
/* -------------------------------------------------------------------------- */

int main(void) {
  vcom_log("# galapagos BLE anchor and hardware GPIO-event reporter\n");

  int error = bt_enable(NULL);

  if (error != 0) {
    vcom_log("FATAL,%s,BT_ENABLE,%d\n", NODE_NAME, error);
    return 0;
  }

  bt_conn_cb_register(&connection_callbacks);

  error = bt_hci_register_vnd_evt_cb(anchor_point_event);

  if (error != 0) {
    vcom_log("FATAL,%s,HCI_VS_CALLBACK,%d\n", NODE_NAME, error);
    return 0;
  }

  sdc_hci_cmd_vs_conn_anchor_point_update_event_report_enable_t enable = {
      .enable = true,
  };

  error = hci_vs_sdc_conn_anchor_point_update_event_report_enable(&enable);

  if (error != 0) {
    vcom_log("FATAL,%s,ANCHOR_REPORT_ENABLE,%d\n", NODE_NAME, error);
    return 0;
  }

  error = configure_external_event_capture();

  if (error != 0) {
    vcom_log("FATAL,%s,EVENT_CAPTURE_CONFIG,%d\n", NODE_NAME, error);
    return 0;
  }

  error = configure_ttl_output();

  if (error != 0) {
    vcom_log("FATAL,%s,TTL_OUTPUT_CONFIG,%d\n", NODE_NAME, error);
    return 0;
  }

  error = advertising_start();

  if (error != 0) {
    return 0;
  }

  vcom_log(
      "READY,%s,%u,%u,HARDWARE_CAPTURE,event_pin=%u,ttl_output_pin=%u,grtc_"
      "ch=%u\n",
      NODE_NAME, REPORT_RATE_HZ, SYNC_FRAME_SIZE, (unsigned int)EVENT_INPUT_PIN,
      (unsigned int)TTL_OUTPUT_PIN, (unsigned int)event_grtc_channel);

  while (true) {
    k_sleep(K_SECONDS(60));
  }

  return 0;
}