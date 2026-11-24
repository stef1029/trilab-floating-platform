/*
 * hub - nRF52840 synchronization hub
 *
 * nRF Connect SDK 3.4.0
 * Board target: nrf52840dk/nrf52840
 *
 * Concurrent timing paths:
 *
 *  1. node (STM32G071 over half-duplex RS-485 + hardware SYNC GPIO)
 * TIMER2 COMPARE0 -> PPI -> GPIOTE CLR
 * TIMER2 COMPARE1 -> PPI -> GPIOTE SET
 *  2. ttl_generator (nRF54L15 over BLE)
 *     Both controllers report Bluetooth connection-event anchor timestamps.
 *     ttl_generator sends its anchor in the same 40-byte frame used by the STM32.
 *     hub matches the 16-bit connection-event counter to its own anchor.
 *
 * Every node owns an independent 16-point affine model. All streamed records
 * put the node name immediately after the record type, for example:
 *
 *   SYNC,node,...
 *   SYNC,ttl_generator,...
 *   MODEL_RESET,ttl_generator,...
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/notify.h>
#include <zephyr/sys/onoff.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <bluetooth/hci_vs_sdc.h>
#include <gpiote_nrfx.h>
#include <hal/nrf_clock.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_rtc.h>
#include <hal/nrf_timer.h>
#include <helpers/nrfx_gppi.h>
#include <nrfx_gpiote.h>
#include <nrfx_rtc.h>
#include <nrfx_timer.h>
#include <soc.h>

#include "command_protocol.h"

/* Keep Command protocol compatibility while making Hub transport-neutral. */
#define HOST_STATUS_NODE_TRANSPORT_FAILED HOST_STATUS_I2C_WRITE_FAILED

#define NODE_HUB "hub"
#define NODE_NODE "node"
#define NODE_TTL_GENERATOR "ttl_generator"
#define NODE_HOST "adelie"
#define TTL_GENERATOR_ADVERTISED_NAME "ttl_generator"

#define USER_NODE DT_PATH(zephyr_user)
#define TIMER_NODE DT_NODELABEL(timer2)
#define TTL_CAPTURE_TIMER_NODE DT_NODELABEL(timer3)
#define RTC_NODE DT_NODELABEL(rtc2)
#define NODE_UART_NODE DT_NODELABEL(uart1)

BUILD_ASSERT(DT_NODE_HAS_PROP(USER_NODE, sync_gpios),
             "The board overlay must define zephyr,user sync-gpios");
BUILD_ASSERT(DT_NODE_HAS_PROP(USER_NODE, event_gpios),
             "The board overlay must define zephyr,user event-gpios");
BUILD_ASSERT(DT_NODE_HAS_PROP(USER_NODE, ttl_input_gpios),
             "The board overlay must define zephyr,user ttl-input-gpios");
BUILD_ASSERT(DT_NODE_HAS_STATUS(TIMER_NODE, okay),
             "TIMER2 must be enabled by the board overlay");
BUILD_ASSERT(DT_NODE_HAS_STATUS(TTL_CAPTURE_TIMER_NODE, okay),
             "TIMER3 must be enabled by the board overlay");
BUILD_ASSERT(DT_NODE_HAS_STATUS(RTC_NODE, okay),
             "RTC2 must be enabled by the board overlay");
BUILD_ASSERT(DT_NODE_HAS_PROP(USER_NODE, node_rs485_de_gpios),
             "The board overlay must define zephyr,user node-rs485-de-gpios");
BUILD_ASSERT(DT_NODE_HAS_STATUS(NODE_UART_NODE, okay),
             "UARTE1 must be enabled by the board overlay");

#define SYNC_OUTPUT_PIN NRF_DT_GPIOS_TO_PSEL(USER_NODE, sync_gpios)
#define EVENT_INPUT_PIN NRF_DT_GPIOS_TO_PSEL(USER_NODE, event_gpios)
#define TTL_INPUT_PIN NRF_DT_GPIOS_TO_PSEL(USER_NODE, ttl_input_gpios)
#define NODE_RS485_DE_PIN NRF_DT_GPIOS_TO_PSEL(USER_NODE, node_rs485_de_gpios)
#define GPIOTE_NODE NRF_DT_GPIOTE_NODE(USER_NODE, sync_gpios)
#define EVENT_GPIOTE_NODE NRF_DT_GPIOTE_NODE(USER_NODE, event_gpios)
#define TTL_GPIOTE_NODE NRF_DT_GPIOTE_NODE(USER_NODE, ttl_input_gpios)

BUILD_ASSERT(DT_SAME_NODE(GPIOTE_NODE, EVENT_GPIOTE_NODE),
             "SYNC and event GPIOs must use the same GPIOTE instance");
BUILD_ASSERT(DT_SAME_NODE(GPIOTE_NODE, TTL_GPIOTE_NODE),
             "SYNC and TTL GPIOs must use the same GPIOTE instance");

#define NODE_RS485_ADDRESS 1U
#define NODE_RS485_BAUD 460800U

#define NODE_RS485_REQUEST_MAGIC_0 0xA6U
#define NODE_RS485_REQUEST_MAGIC_1 0x5AU
#define NODE_RS485_RESPONSE_MAGIC_0 0xA6U
#define NODE_RS485_RESPONSE_MAGIC_1 0xA5U
#define NODE_RS485_OPCODE_POLL 0x10U
#define NODE_RS485_OPCODE_COMMAND 0x11U
#define NODE_RS485_RESPONSE_BIT 0x80U
#define NODE_RS485_NO_ACK 0xFFU
#define NODE_RS485_REQUEST_SIZE 8U
#define NODE_RS485_COMMAND_RESPONSE_SIZE 8U
#define NODE_RS485_POLL_RESPONSE_SIZE (6U + REMOTE_FRAME_SIZE + 2U)
#define NODE_RS485_MAX_RESPONSE_SIZE NODE_RS485_POLL_RESPONSE_SIZE
#define NODE_RS485_RETRY_COUNT 2U
#define NODE_RS485_TX_TIMEOUT_US 5000
#define NODE_RS485_RX_IDLE_TIMEOUT_US 1500
#define NODE_RS485_TRANSACTION_TIMEOUT_MS 12
#define NODE_RS485_RX_DISABLE_TIMEOUT_MS 2

#define HUB_TIMER_HZ 16000000U
#define NODE_TIMER_NOMINAL_HZ 16000000ULL
#define HUB_SYNC_RATE_HZ 4U
#define HUB_SYNC_PERIOD_TICKS (HUB_TIMER_HZ / HUB_SYNC_RATE_HZ)
#define NODE_SYNC_INTERVAL_NOMINAL_TICKS                                      \
  (NODE_TIMER_NOMINAL_HZ / HUB_SYNC_RATE_HZ)
#define HUB_SYNC_PULSE_WIDTH_TICKS 1600U /* 100 us */
#define NODE_SYNC_ACCEPTANCE_WINDOW_TICKS (HUB_SYNC_PERIOD_TICKS / 2U)
#define NODE_FIRST_POLL_TICKS 32000U         /* 2 ms */
#define NODE_POLL_INTERVAL_TICKS 160000U     /* 10 ms */
#define NODE_FINAL_DRAIN_GUARD_TICKS 400000U /* 25 ms */
#define NODE_FINAL_DRAIN_PHASE_TICKS                                          \
  (HUB_SYNC_PERIOD_TICKS - NODE_FINAL_DRAIN_GUARD_TICKS)
#define NODE_LOCAL_ACCEPTANCE_MARGIN_TICKS 200000ULL /* 12.5 ms */
#define NODE_LOCAL_ACCEPTANCE_WINDOW_TICKS                                    \
  ((NODE_SYNC_INTERVAL_NOMINAL_TICKS / 2ULL) +                                \
   NODE_LOCAL_ACCEPTANCE_MARGIN_TICKS)

#define NODE_PAIRING_MAX_RATE_ERROR_PPM 25000ULL
#define NODE_PAIRING_MAX_TRANSPORT_TICKS 320000ULL /* 20 ms */
#define NODE_PAIRING_AGE_MARGIN_TICKS 32000ULL     /* 2 ms */

#define CLOCK_MODEL_WINDOW_SIZE 16U

#ifndef CLOCK_MODEL_MAX_SKEW_PPM
#define CLOCK_MODEL_MAX_SKEW_PPM 20000.0
#endif

/* Defaults suitable for the current 4 Hz HSI16 bring-up. */
#ifndef CLOCK_MODEL_ACQUIRE_RMS_LIMIT_TICKS
#define CLOCK_MODEL_ACQUIRE_RMS_LIMIT_TICKS 1600.0 /* 100 us */
#endif
#ifndef CLOCK_MODEL_TRACK_RMS_LIMIT_TICKS
#define CLOCK_MODEL_TRACK_RMS_LIMIT_TICKS 1600.0 /* 100 us */
#endif
#ifndef CLOCK_MODEL_ADMISSION_FLOOR_TICKS
#define CLOCK_MODEL_ADMISSION_FLOOR_TICKS 3200.0 /* 200 us */
#endif
#ifndef CLOCK_MODEL_MAX_UPDATE_STEP_TICKS
#define CLOCK_MODEL_MAX_UPDATE_STEP_TICKS 1600.0 /* 100 us */
#endif

#define CLOCK_MODEL_ADMISSION_RMS_MULTIPLIER 6.0
#define CLOCK_MODEL_MAX_CONSECUTIVE_REJECTIONS 3U

#define NODE_TIMEOUT_HISTORY_SIZE 60U
#define NODE_MAX_TIMEOUTS_IN_HISTORY 5U
#define NODE_MAX_CONSECUTIVE_TIMEOUTS 3U
#define NODE_MAX_CONSECUTIVE_BAD_WINDOWS 3U
#define NODE_MAX_CONSECUTIVE_TRANSPORT_ERRORS 3U

/* Exact node frame layout, reused by BLE notifications. */
#define REMOTE_FRAME_MAGIC 0xA5U
#define REMOTE_FRAME_VERSION 2U
#define REMOTE_FRAME_SIZE 40U

#define REMOTE_OFFSET_MAGIC 0U
#define REMOTE_OFFSET_VERSION 1U
#define REMOTE_OFFSET_STATUS_FLAGS 2U
#define REMOTE_OFFSET_LENGTH 3U
#define REMOTE_OFFSET_RECORD_TYPE 4U
#define REMOTE_OFFSET_PENDING_COUNT 5U
#define REMOTE_OFFSET_RECORD_FLAGS 6U
#define REMOTE_OFFSET_CAPTURE_TICKS 8U
#define REMOTE_OFFSET_SNAPSHOT_TICKS 16U
#define REMOTE_OFFSET_CAPTURE_LOSS_COUNT 24U
#define REMOTE_OFFSET_TRANSPORT_ERROR_COUNT 28U
#define REMOTE_OFFSET_AUXILIARY 32U
#define REMOTE_OFFSET_CRC 36U

#define REMOTE_STATUS_RECORD_VALID BIT(0)
#define REMOTE_STATUS_FIRST_AFTER_RESET BIT(1)
#define REMOTE_STATUS_CAPTURE_LOSS_LATCHED BIT(2)
#define REMOTE_STATUS_TRANSPORT_ERROR_LATCHED BIT(3)
#define REMOTE_STATUS_CLOCK_FAULT BIT(4)

#define REMOTE_RECORD_NONE 0U
#define REMOTE_RECORD_SYNC 1U
#define REMOTE_RECORD_EVENT 2U
#define REMOTE_RECORD_COMMAND_ACK 3U
#define REMOTE_RECORD_TTL_GENERATED 4U

#define REMOTE_RECORD_FLAG_HW_OVERCAPTURE BIT(0)

#define NODE_COMMAND_ACK_RESET_SEEN 0x01U
#define NODE_COMMAND_CLEAR_LATCHED_FLAGS 0x02U
#define NODE_COMMAND_DO_NOW_MASK 0x80U
#define NODE_COMMAND_TOKEN_MASK 0x7FU

#define BT_UUID_HOST_SERVICE_VAL                                             \
  BT_UUID_128_ENCODE(0xA88279D0, 0x7009, 0x4BEE, 0xA6F8, 0xE1DC3FF02B92)
#define BT_UUID_HOST_SERVICE BT_UUID_DECLARE_128(BT_UUID_HOST_SERVICE_VAL)
#define BT_UUID_HOST_RX_VAL                                                  \
  BT_UUID_128_ENCODE(0xA88279D1, 0x7009, 0x4BEE, 0xA6F8, 0xE1DC3FF02B92)
#define BT_UUID_HOST_RX BT_UUID_DECLARE_128(BT_UUID_HOST_RX_VAL)
#define BT_UUID_HOST_TX_VAL                                                  \
  BT_UUID_128_ENCODE(0xA88279D2, 0x7009, 0x4BEE, 0xA6F8, 0xE1DC3FF02B92)
#define BT_UUID_HOST_TX BT_UUID_DECLARE_128(BT_UUID_HOST_TX_VAL)

#define HOST_REQUEST_QUEUE_DEPTH 8U
#define HOST_COMMAND_TIMEOUT_TICKS (HUB_TIMER_HZ * 2ULL)

#define HOST_NOTIFY_QUEUE_DEPTH 16U
#define HOST_NOTIFY_RETRY_DELAY_MS 5U
#define HOST_NOTIFY_COMPLETE_TIMEOUT_MS 2000U

/* Canonical stream schema. */
#define STREAM_SCHEMA_VERSION 3U
#define EVENT_KIND_GPIO_RISE "GPIO_RISE"
#define EVENT_KIND_CLOCK_SYNC_TX "CLOCK_SYNC_TX"
#define EVENT_KIND_CLOCK_SYNC_RX "CLOCK_SYNC_RX"
#define EVENT_KIND_CLOCK_SYNC_REPLY_QUEUE "CLOCK_SYNC_REPLY_QUEUE"
#define EVENT_KIND_CLOCK_SYNC_REPLY_TX "CLOCK_SYNC_REPLY_TX"
#define EVENT_KIND_COMMAND_TX "COMMAND_TX"
#define EVENT_KIND_COMMAND_RX "COMMAND_RX"
#define EVENT_KIND_COMMAND_FORWARD "COMMAND_FORWARD"
#define EVENT_KIND_COMMAND_EXEC "COMMAND_EXEC"
#define EVENT_KIND_COMMAND_ACK_RX "COMMAND_ACK_RX"
#define EVENT_KIND_COMMAND_RESULT_QUEUE "COMMAND_RESULT_QUEUE"
#define EVENT_KIND_COMMAND_RESULT_TX "COMMAND_RESULT_TX"
#define EVENT_KIND_TTL_PULSE_GENERATED "TTL_PULSE_GENERATED"
#define EVENT_KIND_TTL_PULSE_ACQUIRED "TTL_PULSE_ACQUIRED"

#define EVENT_STATE_LOCAL "LOCAL"
#define EVENT_STATE_TRACK "TRACK"
#define EVENT_STATE_UNSYNC "UNSYNC"
#define EVENT_STATE_REMOTE "REMOTE"

#define HOST_CLOCK_HZ 1000000000ULL
#define TTL_GENERATOR_EVENT_CLOCK_HZ 16000000ULL
#define HUB_EVENT_QUEUE_DEPTH 16U
#define HUB_EVENT_CACHE_SIZE 32U
#define HUB_EVENT_MATCH_LIMIT_TICKS (HUB_TIMER_HZ / 100U) /* 10 ms */

struct host_notification {
  uint8_t frame[HOST_FRAME_SIZE];
  uint8_t type;
  uint32_t sequence;
  uint64_t completed_ticks;
};

K_MSGQ_DEFINE(host_notify_queue, sizeof(struct host_notification),
              HOST_NOTIFY_QUEUE_DEPTH, 4);

K_SEM_DEFINE(host_notify_complete_sem, 0, 1);

/* Custom hub synchronization service and anchor-report UUIDs. */
#define BT_UUID_HUB_SYNC_SERVICE_VAL                                        \
  BT_UUID_128_ENCODE(0xA88278D0, 0x7009, 0x4BEE, 0xA6F8, 0xE1DC3FF02B92)
#define BT_UUID_HUB_SYNC_SERVICE                                            \
  BT_UUID_DECLARE_128(BT_UUID_HUB_SYNC_SERVICE_VAL)

#define BT_UUID_HUB_TTL_CONTROL_VAL                                         \
  BT_UUID_128_ENCODE(0xA88278D1, 0x7009, 0x4BEE, 0xA6F8, 0xE1DC3FF02B92)
#define BT_UUID_HUB_TTL_CONTROL                                             \
  BT_UUID_DECLARE_128(BT_UUID_HUB_TTL_CONTROL_VAL)

#define BT_UUID_HUB_ANCHOR_REPORT_VAL                                       \
  BT_UUID_128_ENCODE(0xA88278D2, 0x7009, 0x4BEE, 0xA6F8, 0xE1DC3FF02B92)
#define BT_UUID_HUB_ANCHOR_REPORT                                           \
  BT_UUID_DECLARE_128(BT_UUID_HUB_ANCHOR_REPORT_VAL)

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

#define TTL_TEST_LEAD_MS 300U
#define TTL_TEST_PERIOD_MS 1000U
#define TTL_TEST_PULSE_WIDTH_US 100U
#define TTL_TEST_TIMEOUT_AFTER_TARGET_MS 250U
#define TTL_TEST_RETRY_MS 500U
#define TTL_CAPTURE_QUEUE_DEPTH 8U

#define TTL_GENERATOR_ANCHOR_CACHE_SIZE 64U
#define TTL_GENERATOR_PAIR_QUEUE_DEPTH 16U
#define TTL_GENERATOR_CONTROLLER_TICKS_PER_US 16ULL
#define TTL_GENERATOR_DEFAULT_CONN_INTERVAL_US 10000U

BUILD_ASSERT((HUB_TIMER_HZ % HUB_SYNC_RATE_HZ) == 0U,
             "SYNC rate must divide the hub timer frequency");
BUILD_ASSERT((NODE_TIMER_NOMINAL_HZ % HUB_SYNC_RATE_HZ) == 0ULL,
             "SYNC rate must divide the node timer frequency");
BUILD_ASSERT(HUB_SYNC_PULSE_WIDTH_TICKS < NODE_SYNC_ACCEPTANCE_WINDOW_TICKS,
             "SYNC pulse must end inside the acceptance window");
BUILD_ASSERT(NODE_SYNC_ACCEPTANCE_WINDOW_TICKS < NODE_FINAL_DRAIN_PHASE_TICKS,
             "Dead-zone polling must remain possible");
BUILD_ASSERT(NODE_FINAL_DRAIN_GUARD_TICKS >
                 (NODE_PAIRING_MAX_TRANSPORT_TICKS +
                  NODE_PAIRING_AGE_MARGIN_TICKS),
             "Final guard must exceed transport allowance");
BUILD_ASSERT(CLOCK_MODEL_WINDOW_SIZE >= 2U,
             "At least two points are required for a fit");
BUILD_ASSERT(REMOTE_FRAME_SIZE == (REMOTE_OFFSET_CRC + 4U),
             "Frame offsets do not fit the frame size");
BUILD_ASSERT((TTL_GENERATOR_ANCHOR_CACHE_SIZE &
              (TTL_GENERATOR_ANCHOR_CACHE_SIZE - 1U)) == 0U,
             "BLE anchor cache size must be a power of two");

struct remote_frame {
  uint8_t status_flags;
  uint8_t record_type;
  uint8_t pending_count;
  uint8_t record_flags;
  uint64_t capture_ticks;
  uint64_t snapshot_ticks;
  uint32_t capture_loss_count;
  uint32_t transport_error_count;
  uint32_t auxiliary;
};

struct clock_model_point {
  uint64_t local_ticks;
  uint64_t hub_ticks;
};

struct clock_fit_result {
  double slope;
  uint64_t local_reference;
  double hub_at_local_reference;
  double rms_ticks;
};

struct clock_model {
  struct clock_model_point points[CLOCK_MODEL_WINDOW_SIZE];
  size_t count;
  bool valid;
  double slope;
  uint64_t local_reference;
  double hub_at_local_reference;
  double rms_ticks;
};

struct sync_node {
  const char *name;
  uint64_t nominal_interval_ticks;
  struct clock_model model;
  bool have_previous_pair;
  uint32_t previous_pulse_number;
  uint64_t previous_local_ticks;
  unsigned int consecutive_admission_rejections;
};

struct node_sync_window {
  bool open;
  bool candidate_valid;
  bool invalid;
  uint8_t sync_reports_seen;
  uint32_t pulse_number;
  uint64_t hub_ticks;
  struct remote_frame candidate;
};

struct ttl_generator_anchor_slot {
  bool central_valid;
  bool peripheral_valid;
  uint16_t event_counter;
  uint32_t generation;
  uint64_t central_controller_ticks;
  struct remote_frame peripheral;
};

struct ttl_generator_pair {
  uint16_t event_counter;
  uint32_t generation;
  uint64_t central_controller_ticks;
  struct remote_frame peripheral;
};

enum ttl_generator_discovery_stage {
  TTL_GENERATOR_DISCOVERY_NONE = 0,
  TTL_GENERATOR_DISCOVERY_SERVICE,
  TTL_GENERATOR_DISCOVERY_TTL_CHARACTERISTIC,
  TTL_GENERATOR_DISCOVERY_REPORT_CHARACTERISTIC,
  TTL_GENERATOR_DISCOVERY_CCC,
};

struct ttl_generator_client_state {
  struct bt_conn *conn;
  uint8_t conn_index;
  enum ttl_generator_discovery_stage discovery_stage;
  uint16_t service_end_handle;
  uint16_t ttl_control_handle;
  uint16_t value_handle;
  struct bt_gatt_discover_params discover;
  struct bt_gatt_subscribe_params subscribe;
  struct bt_gatt_exchange_params exchange;
};

struct host_request {
  uint8_t type;
  uint32_t sequence;
  uint64_t host_t1_ns;
  uint32_t value;
  uint64_t hub_rx_ticks;
};

struct host_command_state {
  bool active;
  uint32_t sequence;
  uint8_t node_token;
  uint64_t deadline_ticks;
  uint64_t hub_rx_ticks;
  uint64_t node_tx_start_ticks;
};

struct hub_event_record {
  uint32_t event_id;
  uint64_t hub_ticks;
};

struct hub_event_reference {
  uint32_t event_id;
  uint64_t hub_ticks;
};

struct ttl_capture_record {
  uint32_t sequence;
  uint64_t target_hub_ticks;
  uint64_t acquired_hub_ticks;
};

struct ttl_test_state {
  bool pending;
  bool generated_seen;
  bool generated_logged;
  bool acquired_seen;
  bool acquired_logged;
  bool result_logged;
  uint32_t sequence;
  uint64_t target_hub_ticks;
  uint64_t target_local_ticks;
  uint64_t generated_local_ticks;
  int64_t generated_hub_ticks;
  uint64_t acquired_hub_ticks;
};

static const struct device *const node_uart = DEVICE_DT_GET(NODE_UART_NODE);

K_MUTEX_DEFINE(node_rs485_mutex);
K_SEM_DEFINE(node_rs485_tx_done_sem, 0, 1);
K_SEM_DEFINE(node_rs485_rx_done_sem, 0, 1);
K_SEM_DEFINE(node_rs485_rx_disabled_sem, 0, 1);

static volatile size_t node_rs485_expected_rx_length;
static volatile size_t node_rs485_received_length;
static volatile int node_rs485_async_error;
static uint8_t node_rs485_next_bus_sequence;
static bool node_rs485_have_poll_ack;
static uint8_t node_rs485_poll_ack_sequence;

static nrfx_timer_t hub_timer = NRFX_TIMER_INSTANCE(NRF_TIMER2);
static nrfx_timer_t ttl_capture_timer = NRFX_TIMER_INSTANCE(NRF_TIMER3);
static nrfx_gpiote_t *const hub_gpiote =
    &GPIOTE_NRFX_INST_BY_NODE(GPIOTE_NODE);
static nrfx_gppi_handle_t hub_sync_assert_connection;
static nrfx_gppi_handle_t hub_sync_release_connection;
static uint8_t hub_sync_output_channel;
static nrfx_gppi_handle_t hub_event_capture_connection;
static uint8_t hub_event_input_channel;
static nrfx_gppi_handle_t ttl_timer_clear_connection;
static nrfx_gppi_handle_t ttl_input_capture_connection;
static uint8_t ttl_input_gpiote_channel;

static atomic_t hub_sync_pulse_count;
static atomic_t hub_event_sequence;
static atomic_t hub_event_drop_count;
static atomic_t node_event_sequence;
static uint32_t node_last_opened_pulse;

static struct k_spinlock hub_event_cache_lock;
static struct hub_event_reference
    hub_event_cache[HUB_EVENT_CACHE_SIZE];
static size_t hub_event_cache_next;

K_MSGQ_DEFINE(hub_event_queue, sizeof(struct hub_event_record),
              HUB_EVENT_QUEUE_DEPTH, 4);

K_MSGQ_DEFINE(ttl_capture_queue, sizeof(struct ttl_capture_record),
              TTL_CAPTURE_QUEUE_DEPTH, 4);

static struct k_spinlock ttl_test_lock;
static struct ttl_test_state ttl_test;
static atomic_t ttl_test_sequence = ATOMIC_INIT(0);

static struct sync_node node_node = {
    .name = NODE_NODE,
    .nominal_interval_ticks = NODE_SYNC_INTERVAL_NOMINAL_TICKS,
};

static struct sync_node ttl_generator_node = {
    .name = NODE_TTL_GENERATOR,
    .nominal_interval_ticks = (uint64_t)TTL_GENERATOR_DEFAULT_CONN_INTERVAL_US *
                              TTL_GENERATOR_CONTROLLER_TICKS_PER_US,
};

static struct clock_model ttl_generator_controller_bridge;
static bool ttl_generator_controller_bridge_announced;

static struct node_sync_window node_sync_window_state;

static bool node_reset_latch_handled;
static bool node_capture_loss_latch_handled;
static bool node_transport_error_latch_handled;

static uint8_t node_timeout_history[NODE_TIMEOUT_HISTORY_SIZE];
static size_t node_timeout_history_index;
static size_t node_timeout_history_count;
static unsigned int node_timeout_history_sum;
static unsigned int node_consecutive_timeouts;
static unsigned int node_consecutive_bad_windows;
static unsigned int node_consecutive_transport_errors;

static struct onoff_client hub_hfclk_client;
static bool hub_hfclk_reserved;

static nrfx_rtc_t ttl_generator_controller_rtc = NRFX_RTC_INSTANCE(NRF_RTC2);
static volatile uint32_t ttl_generator_controller_rtc_overflows;
static uint32_t ttl_generator_controller_to_app_rtc_offset_ticks;

static struct ttl_generator_client_state ttl_generator_client;
static atomic_t ttl_generator_connecting;
static atomic_t ttl_generator_connection_active;
static atomic_t ttl_generator_connection_generation;
K_MUTEX_DEFINE(ttl_generator_state_mutex);
static struct k_spinlock ttl_generator_cache_lock;
static struct ttl_generator_anchor_slot
    ttl_generator_anchor_cache[TTL_GENERATOR_ANCHOR_CACHE_SIZE];
static bool ttl_generator_have_unwrapped_counter;
static uint16_t ttl_generator_previous_event_counter;
static uint32_t ttl_generator_unwrapped_event_counter;

static struct bt_conn *host_connection;
K_MUTEX_DEFINE(host_connection_mutex);
static atomic_t host_notifications_enabled;
static struct host_command_state host_command;

K_MSGQ_DEFINE(host_request_queue, sizeof(struct host_request),
              HOST_REQUEST_QUEUE_DEPTH, 4);

K_MSGQ_DEFINE(ttl_generator_pair_queue, sizeof(struct ttl_generator_pair),
              TTL_GENERATOR_PAIR_QUEUE_DEPTH, 4);

static void node_window_open_work_handler(struct k_work *work);
static void node_poll_work_handler(struct k_work *work);
static void node_window_close_work_handler(struct k_work *work);
static void ttl_generator_pair_work_handler(struct k_work *work);
static void ttl_generator_scan_restart_work_handler(struct k_work *work);
static void host_request_work_handler(struct k_work *work);
static void host_command_timeout_work_handler(struct k_work *work);
static void hub_event_work_handler(struct k_work *work);
static void ttl_capture_work_handler(struct k_work *work);
static void ttl_result_work_handler(struct k_work *work);
static void ttl_schedule_work_handler(struct k_work *work);
static void ttl_timeout_work_handler(struct k_work *work);
static void ttl_input_gpio_handler(nrfx_gpiote_pin_t pin,
                                   nrfx_gpiote_trigger_t trigger,
                                   void *context);
static void hub_event_gpio_handler(nrfx_gpiote_pin_t pin,
                                      nrfx_gpiote_trigger_t trigger,
                                      void *context);
static int hub_gpiote_init(void);
static int hub_event_input_init(void);
static int hub_ttl_input_init(void);

static uint64_t hub_time_now_ticks(void);
static uint32_t hub_timer_phase_ticks(void);
static double clock_model_predict(const struct clock_model *model,
                                  uint64_t local_ticks);
static void stream_log_event(const char *node, uint32_t event_id,
                             const char *kind, uint64_t local_ticks,
                             uint64_t local_hz, int64_t hub_ticks,
                             const char *state, uint64_t transport_age_ticks);
static void stream_log_link(const char *node, const char *transport,
                            const char *role, const char *state,
                            const char *peer, uint32_t interval_us,
                            uint32_t reason);
static void stream_log_fault(const char *node, const char *category,
                             int32_t code, uint64_t value);
static int node_write_command(uint8_t command);
static int node_read_frame(struct remote_frame *snapshot);
static void node_handle_status(const struct remote_frame *snapshot);
static void node_process_frame(const struct remote_frame *snapshot,
                                uint32_t pulse_before_read,
                                uint32_t pulse_after_read,
                                uint32_t phase_after_read);
static ssize_t host_request_write(struct bt_conn *conn,
                                    const struct bt_gatt_attr *attr,
                                    const void *buffer, uint16_t length,
                                    uint16_t offset, uint8_t flags);
static void host_subscription_changed(const struct bt_gatt_attr *attr,
                                        uint16_t value);

K_WORK_DEFINE(node_window_open_work, node_window_open_work_handler);
K_WORK_DELAYABLE_DEFINE(node_poll_work, node_poll_work_handler);
K_WORK_DELAYABLE_DEFINE(node_window_close_work,
                        node_window_close_work_handler);
K_WORK_DEFINE(ttl_generator_pair_work, ttl_generator_pair_work_handler);
K_WORK_DEFINE(ttl_generator_scan_restart_work, ttl_generator_scan_restart_work_handler);
K_WORK_DEFINE(host_request_work, host_request_work_handler);
K_WORK_DELAYABLE_DEFINE(host_command_timeout_work,
                        host_command_timeout_work_handler);
K_WORK_DEFINE(hub_event_work, hub_event_work_handler);
K_WORK_DEFINE(ttl_capture_work, ttl_capture_work_handler);
K_WORK_DEFINE(ttl_result_work, ttl_result_work_handler);
K_WORK_DELAYABLE_DEFINE(ttl_schedule_work, ttl_schedule_work_handler);
K_WORK_DELAYABLE_DEFINE(ttl_timeout_work, ttl_timeout_work_handler);

static void host_notification_complete(struct bt_conn *conn,
                                         void *user_data) {
  ARG_UNUSED(conn);

  struct host_notification *notification = user_data;

  if (notification != NULL) {
    notification->completed_ticks = hub_time_now_ticks();
  }

  k_sem_give(&host_notify_complete_sem);
}

BT_GATT_SERVICE_DEFINE(
    host_service, BT_GATT_PRIMARY_SERVICE(BT_UUID_HOST_SERVICE),
    BT_GATT_CHARACTERISTIC(BT_UUID_HOST_RX, BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE, NULL, host_request_write,
                           NULL),
    BT_GATT_CHARACTERISTIC(BT_UUID_HOST_TX, BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(host_subscription_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

static void host_frame_encode(uint8_t frame[HOST_FRAME_SIZE], uint8_t type,
                                uint32_t sequence, uint64_t timestamp,
                                uint32_t value) {
  memset(frame, 0, HOST_FRAME_SIZE);
  sys_put_le16(HOST_FRAME_MAGIC, &frame[HOST_OFFSET_MAGIC]);
  frame[HOST_OFFSET_VERSION] = HOST_FRAME_VERSION;
  frame[HOST_OFFSET_TYPE] = type;
  sys_put_le32(sequence, &frame[HOST_OFFSET_SEQUENCE]);
  sys_put_le64(timestamp, &frame[HOST_OFFSET_TIMESTAMP]);
  sys_put_le32(value, &frame[HOST_OFFSET_VALUE]);
}

static int host_notification_enqueue(uint8_t type, uint32_t sequence,
                                       uint64_t timestamp, uint32_t value) {
  if (atomic_get(&host_notifications_enabled) == 0) {
    return -EACCES;
  }

  struct host_notification notification = {
      .type = type,
      .sequence = sequence,
  };

  host_frame_encode(notification.frame, type, sequence, timestamp, value);

  const int error = k_msgq_put(&host_notify_queue, &notification, K_NO_WAIT);

  if (error != 0) {
    stream_log_fault(NODE_HOST, "NOTIFY_QUEUE_FULL", error, sequence);

    return -ENOSPC;
  }

  return 0;
}
static void host_error_enqueue(uint32_t sequence, uint32_t status) {
  (void)host_notification_enqueue(HOST_MSG_ERROR, sequence,
                                    hub_time_now_ticks(), status);
}

static ssize_t host_request_write(struct bt_conn *conn,
                                    const struct bt_gatt_attr *attr,
                                    const void *buffer, uint16_t length,
                                    uint16_t offset, uint8_t flags) {
  ARG_UNUSED(conn);
  ARG_UNUSED(attr);
  ARG_UNUSED(flags);

  if ((offset != 0U) || (length != HOST_FRAME_SIZE)) {
    return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
  }

  const uint8_t *frame = buffer;
  if ((sys_get_le16(&frame[HOST_OFFSET_MAGIC]) != HOST_FRAME_MAGIC) ||
      (frame[HOST_OFFSET_VERSION] != HOST_FRAME_VERSION)) {
    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
  }

  const uint8_t type = frame[HOST_OFFSET_TYPE];
  if ((type != HOST_MSG_CLOCK_SYNC_REQUEST) &&
      (type != HOST_MSG_NODE_DO_NOW_REQUEST)) {
    return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
  }

  const struct host_request request = {
      .type = type,
      .sequence = sys_get_le32(&frame[HOST_OFFSET_SEQUENCE]),
      .host_t1_ns = sys_get_le64(&frame[HOST_OFFSET_TIMESTAMP]),
      .value = sys_get_le32(&frame[HOST_OFFSET_VALUE]),
      .hub_rx_ticks = hub_time_now_ticks(),
  };

  const bool is_clock_request = request.type == HOST_MSG_CLOCK_SYNC_REQUEST;

  stream_log_event(
      NODE_HOST, request.sequence,
      is_clock_request ? EVENT_KIND_CLOCK_SYNC_TX : EVENT_KIND_COMMAND_TX,
      request.host_t1_ns, HOST_CLOCK_HZ, -1, EVENT_STATE_REMOTE, 0ULL);

  stream_log_event(NODE_HUB, request.sequence,
                   is_clock_request ? EVENT_KIND_CLOCK_SYNC_RX
                                    : EVENT_KIND_COMMAND_RX,
                   request.hub_rx_ticks, HUB_TIMER_HZ,
                   (int64_t)request.hub_rx_ticks, EVENT_STATE_LOCAL, 0ULL);

  if (k_msgq_put(&host_request_queue, &request, K_NO_WAIT) != 0) {
    return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
  }

  (void)k_work_submit(&host_request_work);
  return length;
}

static const char *host_notification_event_kind(uint8_t type) {
  switch (type) {
  case HOST_MSG_CLOCK_REPLY:
    return EVENT_KIND_CLOCK_SYNC_REPLY_TX;
  case HOST_MSG_COMMAND_HUB_DONE_TX:
    return EVENT_KIND_COMMAND_RESULT_TX;
  default:
    return NULL;
  }
}

static void host_notification_thread(void *argument_1, void *argument_2,
                                       void *argument_3) {
  ARG_UNUSED(argument_1);
  ARG_UNUSED(argument_2);
  ARG_UNUSED(argument_3);

  struct host_notification notification;

  while (true) {
    k_msgq_get(&host_notify_queue, &notification, K_FOREVER);

    struct bt_conn *conn = NULL;

    k_mutex_lock(&host_connection_mutex, K_FOREVER);

    if (host_connection != NULL) {
      conn = bt_conn_ref(host_connection);
    }

    k_mutex_unlock(&host_connection_mutex);

    if (conn == NULL) {
      stream_log_fault(NODE_HOST, "NOTIFY_NO_CONNECTION", -ENOTCONN,
                       notification.sequence);
      continue;
    }

    struct bt_gatt_notify_params params = {
        .attr = &host_service.attrs[4],
        .data = notification.frame,
        .len = sizeof(notification.frame),
        .func = host_notification_complete,
        .user_data = &notification,
    };

    int error;

    while (true) {
      k_sem_reset(&host_notify_complete_sem);

      error = bt_gatt_notify_cb(conn, &params);

      if ((error == -ENOMEM) || (error == -EAGAIN)) {
        k_sleep(K_MSEC(HOST_NOTIFY_RETRY_DELAY_MS));
        continue;
      }

      break;
    }

    if (error != 0) {
      stream_log_fault(NODE_HOST, "NOTIFY_SEND", error,
                       notification.sequence);

      bt_conn_unref(conn);
      continue;
    }

    const int wait_error = k_sem_take(
        &host_notify_complete_sem, K_MSEC(HOST_NOTIFY_COMPLETE_TIMEOUT_MS));

    if (wait_error != 0) {
      stream_log_fault(NODE_HOST, "NOTIFY_COMPLETE", wait_error,
                       notification.sequence);
    } else {
      const char *event_kind =
          host_notification_event_kind(notification.type);

      if ((event_kind != NULL) && (notification.completed_ticks != 0ULL)) {
        stream_log_event(NODE_HUB, notification.sequence, event_kind,
                         notification.completed_ticks, HUB_TIMER_HZ,
                         (int64_t)notification.completed_ticks,
                         EVENT_STATE_LOCAL, 0ULL);
      }
    }

    bt_conn_unref(conn);
  }
}

K_THREAD_DEFINE(host_notify_thread_id, 2048, host_notification_thread, NULL,
                NULL, NULL, 7, 0, 0);

static void host_subscription_changed(const struct bt_gatt_attr *attr,
                                        uint16_t value) {
  ARG_UNUSED(attr);

  const bool enabled = value == BT_GATT_CCC_NOTIFY;

  atomic_set(&host_notifications_enabled, enabled ? 1 : 0);

  stream_log_link(NODE_HOST, "BLE", "peripheral",
                  enabled ? "SUBSCRIBED" : "UNSUBSCRIBED", "host", 0U, 0U);
}

static const struct bt_data host_ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1U),
};

static const struct bt_data host_sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_HOST_SERVICE_VAL),
};

static int host_advertising_start(void) {
  const int error =
      bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, host_ad, ARRAY_SIZE(host_ad),
                      host_sd, ARRAY_SIZE(host_sd));
  if ((error != 0) && (error != -EALREADY)) {
    stream_log_fault(NODE_HOST, "ADVERTISE", error, 0ULL);
    return error;
  }
  stream_log_link(NODE_HOST, "BLE", "peripheral", "ADVERTISING", NODE_HUB,
                  0U, 0U);
  return 0;
}

static uint16_t crc16_ccitt(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFFU;

  for (size_t i = 0U; i < length; ++i) {
    crc ^= (uint16_t)data[i] << 8;

    for (unsigned int bit = 0U; bit < 8U; ++bit) {
      if ((crc & 0x8000U) != 0U) {
        crc = (uint16_t)(((uint32_t)crc << 1) ^ 0x1021U);
      } else {
        crc = (uint16_t)((uint32_t)crc << 1);
      }
    }
  }

  return crc;
}

static double math_abs_double(double value) {
  return (value < 0.0) ? -value : value;
}

static double math_sqrt(double value) {
  if (value <= 0.0) {
    return 0.0;
  }

  double estimate = (value > 1.0) ? value : 1.0;

  for (unsigned int i = 0U; i < 40U; ++i) {
    estimate = 0.5 * (estimate + value / estimate);
  }

  return estimate;
}

static int64_t math_round_i64(double value) {
  return (value >= 0.0) ? (int64_t)(value + 0.5) : (int64_t)(value - 0.5);
}

static double tick_delta_double(uint64_t value, uint64_t reference) {
  if (value >= reference) {
    return (double)(value - reference);
  }

  return -(double)(reference - value);
}

static int64_t ticks_to_ns(double ticks) {
  return math_round_i64(ticks * (1000000000.0 / (double)HUB_TIMER_HZ));
}

static int64_t clock_slope_to_ppb(double slope) {
  return math_round_i64((slope - 1.0) * 1000000000.0);
}

static void stream_log_event(const char *node, uint32_t event_id,
                             const char *kind, uint64_t local_ticks,
                             uint64_t local_hz, int64_t hub_ticks,
                             const char *state, uint64_t transport_age_ticks) {
  printk("EVENT,%s,%u,%s,%llu,%llu,%lld,%s,%llu\n", node, event_id, kind,
         (unsigned long long)local_ticks, (unsigned long long)local_hz,
         (long long)hub_ticks, state, (unsigned long long)transport_age_ticks);
}

static void stream_log_event_match(const char *node, uint32_t event_id,
                                   uint32_t reference_event_id,
                                   uint64_t converted_hub_ticks,
                                   uint64_t reference_hub_ticks) {
  const int64_t error_ticks =
      (int64_t)converted_hub_ticks - (int64_t)reference_hub_ticks;

  printk("EVENT_MATCH,%s,%u,%s,%u,%llu,%llu,%lld\n", node, event_id,
         NODE_HUB, reference_event_id,
         (unsigned long long)converted_hub_ticks,
         (unsigned long long)reference_hub_ticks,
         (long long)ticks_to_ns((double)error_ticks));
}

static void stream_log_link(const char *node, const char *transport,
                            const char *role, const char *state,
                            const char *peer, uint32_t interval_us,
                            uint32_t reason) {
  printk("LINK,%s,%s,%s,%s,%s,%u,%u\n", node, transport, role, state, peer,
         interval_us, reason);
}

static void stream_log_fault(const char *node, const char *category,
                             int32_t code, uint64_t value) {
  printk("FAULT,%s,%s,%d,%llu\n", node, category, code,
         (unsigned long long)value);
}

static uint64_t absolute_tick_difference(uint64_t first, uint64_t second) {
  return (first >= second) ? (first - second) : (second - first);
}

static void hub_event_reference_store(uint32_t event_id,
                                         uint64_t hub_ticks) {
  const k_spinlock_key_t key = k_spin_lock(&hub_event_cache_lock);

  hub_event_cache[hub_event_cache_next].event_id = event_id;
  hub_event_cache[hub_event_cache_next].hub_ticks = hub_ticks;
  hub_event_cache_next =
      (hub_event_cache_next + 1U) % HUB_EVENT_CACHE_SIZE;

  k_spin_unlock(&hub_event_cache_lock, key);
}

static bool hub_event_reference_find(uint64_t converted_hub_ticks,
                                        uint32_t *event_id_out,
                                        uint64_t *hub_ticks_out) {
  bool found = false;
  uint64_t best_difference = UINT64_MAX;

  const k_spinlock_key_t key = k_spin_lock(&hub_event_cache_lock);

  for (size_t i = 0U; i < HUB_EVENT_CACHE_SIZE; ++i) {
    if (hub_event_cache[i].event_id == 0U) {
      continue;
    }

    const uint64_t difference = absolute_tick_difference(
        converted_hub_ticks, hub_event_cache[i].hub_ticks);

    if (difference < best_difference) {
      best_difference = difference;
      *event_id_out = hub_event_cache[i].event_id;
      *hub_ticks_out = hub_event_cache[i].hub_ticks;
      found = true;
    }
  }

  k_spin_unlock(&hub_event_cache_lock, key);

  return found && (best_difference <= HUB_EVENT_MATCH_LIMIT_TICKS);
}

static void stream_log_remote_gpio_event(const char *node, uint32_t event_id,
                                         uint64_t local_hz,
                                         struct sync_node *sync_node,
                                         struct k_mutex *model_mutex,
                                         const struct remote_frame *frame) {
  const uint64_t transport_age_ticks =
      frame->snapshot_ticks - frame->capture_ticks;

  bool model_valid;
  int64_t converted_hub_ticks = -1;

  if (model_mutex != NULL) {
    k_mutex_lock(model_mutex, K_FOREVER);
  }

  model_valid = sync_node->model.valid;

  if (model_valid) {
    converted_hub_ticks = math_round_i64(
        clock_model_predict(&sync_node->model, frame->capture_ticks));
  }

  if (model_mutex != NULL) {
    k_mutex_unlock(model_mutex);
  }

  stream_log_event(node, event_id, EVENT_KIND_GPIO_RISE, frame->capture_ticks,
                   local_hz, converted_hub_ticks,
                   model_valid ? EVENT_STATE_TRACK : EVENT_STATE_UNSYNC,
                   transport_age_ticks);

  if (!model_valid || (converted_hub_ticks < 0)) {
    return;
  }

  uint32_t reference_event_id = 0U;
  uint64_t reference_hub_ticks = 0ULL;

  if (hub_event_reference_find((uint64_t)converted_hub_ticks,
                                  &reference_event_id, &reference_hub_ticks)) {
    stream_log_event_match(node, event_id, reference_event_id,
                           (uint64_t)converted_hub_ticks, reference_hub_ticks);
  }
}

static bool remote_frame_decode(const uint8_t frame[REMOTE_FRAME_SIZE],
                                struct remote_frame *snapshot) {
  if ((frame[REMOTE_OFFSET_MAGIC] != REMOTE_FRAME_MAGIC) ||
      (frame[REMOTE_OFFSET_VERSION] != REMOTE_FRAME_VERSION) ||
      (frame[REMOTE_OFFSET_LENGTH] != REMOTE_FRAME_SIZE)) {
    return false;
  }

  const uint16_t received_crc = sys_get_le16(&frame[REMOTE_OFFSET_CRC]);
  const uint16_t computed_crc = crc16_ccitt(frame, REMOTE_OFFSET_CRC);

  if (received_crc != computed_crc) {
    return false;
  }

  snapshot->status_flags = frame[REMOTE_OFFSET_STATUS_FLAGS];
  snapshot->record_type = frame[REMOTE_OFFSET_RECORD_TYPE];
  snapshot->pending_count = frame[REMOTE_OFFSET_PENDING_COUNT];
  snapshot->record_flags = frame[REMOTE_OFFSET_RECORD_FLAGS];
  snapshot->capture_ticks = sys_get_le64(&frame[REMOTE_OFFSET_CAPTURE_TICKS]);
  snapshot->snapshot_ticks = sys_get_le64(&frame[REMOTE_OFFSET_SNAPSHOT_TICKS]);
  snapshot->capture_loss_count =
      sys_get_le32(&frame[REMOTE_OFFSET_CAPTURE_LOSS_COUNT]);
  snapshot->transport_error_count =
      sys_get_le32(&frame[REMOTE_OFFSET_TRANSPORT_ERROR_COUNT]);
  snapshot->auxiliary = sys_get_le32(&frame[REMOTE_OFFSET_AUXILIARY]);

  const bool record_valid =
      (snapshot->status_flags & REMOTE_STATUS_RECORD_VALID) != 0U;

  if (record_valid) {
    if ((snapshot->pending_count == 0U) ||
        ((snapshot->record_type != REMOTE_RECORD_SYNC) &&
         (snapshot->record_type != REMOTE_RECORD_EVENT) &&
         (snapshot->record_type != REMOTE_RECORD_COMMAND_ACK) &&
         (snapshot->record_type != REMOTE_RECORD_TTL_GENERATED)) ||
        (snapshot->snapshot_ticks < snapshot->capture_ticks)) {
      return false;
    }
  } else if ((snapshot->pending_count != 0U) ||
             (snapshot->record_type != REMOTE_RECORD_NONE)) {
    return false;
  }

  return true;
}

static void clock_model_append(struct clock_model *model, uint64_t local_ticks,
                               uint64_t hub_ticks) {
  if (model->count < CLOCK_MODEL_WINDOW_SIZE) {
    model->points[model->count].local_ticks = local_ticks;
    model->points[model->count].hub_ticks = hub_ticks;
    model->count++;
    return;
  }

  memmove(&model->points[0], &model->points[1],
          sizeof(model->points[0]) * (CLOCK_MODEL_WINDOW_SIZE - 1U));
  model->points[CLOCK_MODEL_WINDOW_SIZE - 1U].local_ticks = local_ticks;
  model->points[CLOCK_MODEL_WINDOW_SIZE - 1U].hub_ticks = hub_ticks;
}

static bool clock_model_compute_fit(const struct clock_model *model,
                                    struct clock_fit_result *result) {
  if (model->count < 2U) {
    return false;
  }

  const uint64_t local_reference = model->points[model->count - 1U].local_ticks;
  const uint64_t hub_reference = model->points[model->count - 1U].hub_ticks;

  double mean_x = 0.0;
  double mean_y = 0.0;

  for (size_t i = 0U; i < model->count; ++i) {
    mean_x += tick_delta_double(model->points[i].local_ticks, local_reference);
    mean_y += tick_delta_double(model->points[i].hub_ticks, hub_reference);
  }

  mean_x /= (double)model->count;
  mean_y /= (double)model->count;

  double sum_xx = 0.0;
  double sum_xy = 0.0;

  for (size_t i = 0U; i < model->count; ++i) {
    const double x =
        tick_delta_double(model->points[i].local_ticks, local_reference) -
        mean_x;
    const double y =
        tick_delta_double(model->points[i].hub_ticks, hub_reference) - mean_y;

    sum_xx += x * x;
    sum_xy += x * y;
  }

  if (!(sum_xx > 0.0)) {
    return false;
  }

  result->slope = sum_xy / sum_xx;
  result->local_reference = local_reference;
  result->hub_at_local_reference =
      (double)hub_reference + mean_y - result->slope * mean_x;

  double sum_squared_residual = 0.0;

  for (size_t i = 0U; i < model->count; ++i) {
    const double predicted =
        result->hub_at_local_reference +
        result->slope * tick_delta_double(model->points[i].local_ticks,
                                          result->local_reference);
    const double residual = (double)model->points[i].hub_ticks - predicted;

    sum_squared_residual += residual * residual;
  }

  result->rms_ticks = math_sqrt(sum_squared_residual / (double)model->count);

  return true;
}

static double clock_model_predict(const struct clock_model *model,
                                  uint64_t local_ticks) {
  return model->hub_at_local_reference +
         model->slope * tick_delta_double(local_ticks, model->local_reference);
}

static double clock_fit_predict(const struct clock_fit_result *fit,
                                uint64_t local_ticks) {
  return fit->hub_at_local_reference +
         fit->slope * tick_delta_double(local_ticks, fit->local_reference);
}

static void sync_node_reset(struct sync_node *node, const char *reason) {
  const bool was_active = node->model.valid || (node->model.count != 0U) ||
                          node->have_previous_pair;

  memset(&node->model, 0, sizeof(node->model));
  node->have_previous_pair = false;
  node->consecutive_admission_rejections = 0U;

  if (was_active) {
    printk("MODEL_RESET,%s,%s\n", node->name, reason);
  }
}

static bool sync_node_process_pair(struct sync_node *node,
                                   uint32_t pulse_number, uint64_t hub_ticks,
                                   const struct remote_frame *snapshot) {
  const uint64_t local_ticks = snapshot->capture_ticks;
  const uint64_t transport_age_ticks =
      snapshot->snapshot_ticks - snapshot->capture_ticks;

  uint64_t local_delta = 0ULL;
  int64_t local_interval_error_ticks = 0LL;
  uint32_t emitted_intervals = 0U;

  if (node->have_previous_pair) {
    if (local_ticks <= node->previous_local_ticks) {
      printk("COUNTER_REGRESSION,%s,%u,%llu,%llu\n", node->name, pulse_number,
             (unsigned long long)node->previous_local_ticks,
             (unsigned long long)local_ticks);
      sync_node_reset(node, "counter_regression");
      return false;
    }

    local_delta = local_ticks - node->previous_local_ticks;
    emitted_intervals = pulse_number - node->previous_pulse_number;

    const uint64_t expected_local_delta =
        (uint64_t)emitted_intervals * node->nominal_interval_ticks;

    if (local_delta >= expected_local_delta) {
      local_interval_error_ticks =
          (int64_t)(local_delta - expected_local_delta);
    } else {
      local_interval_error_ticks =
          -(int64_t)(expected_local_delta - local_delta);
    }

    const uint64_t elapsed_local_intervals =
        (local_delta + (node->nominal_interval_ticks / 2ULL)) /
        node->nominal_interval_ticks;

    if (elapsed_local_intervals != (uint64_t)emitted_intervals) {
      printk("DELTA_MISMATCH,%s,%u,%llu,%u,%llu\n", node->name, pulse_number,
             (unsigned long long)local_delta, emitted_intervals,
             (unsigned long long)elapsed_local_intervals);
      sync_node_reset(node, "counter_delta_mismatch");
      return false;
    }
  }

  const size_t prospective_count = node->model.count + 1U;

  printk("PAIR_RAW,%s,%u,%llu,%llu,%u,%u,%llu,%lld,%llu\n", node->name,
         pulse_number, (unsigned long long)hub_ticks,
         (unsigned long long)local_ticks, (unsigned int)prospective_count,
         node->have_previous_pair ? 1U : 0U, (unsigned long long)local_delta,
         (long long)local_interval_error_ticks,
         (unsigned long long)transport_age_ticks);

  const bool had_valid_model = node->model.valid;
  double prefit_residual_ticks = 0.0;
  double model_step_ticks = 0.0;

  if (node->model.valid) {
    const double predicted = clock_model_predict(&node->model, local_ticks);
    prefit_residual_ticks = (double)hub_ticks - predicted;

    double threshold =
        CLOCK_MODEL_ADMISSION_RMS_MULTIPLIER * node->model.rms_ticks;
    if (threshold < CLOCK_MODEL_ADMISSION_FLOOR_TICKS) {
      threshold = CLOCK_MODEL_ADMISSION_FLOOR_TICKS;
    }

    if (math_abs_double(prefit_residual_ticks) > threshold) {
      node->consecutive_admission_rejections++;

      printk("ADMISSION_REJECT,%s,%u,%lld,%lld,%u\n", node->name, pulse_number,
             (long long)ticks_to_ns(prefit_residual_ticks),
             (long long)ticks_to_ns(threshold),
             node->consecutive_admission_rejections);

      if (node->consecutive_admission_rejections >=
          CLOCK_MODEL_MAX_CONSECUTIVE_REJECTIONS) {
        sync_node_reset(node, "repeated_admission_rejection");
      }

      return false;
    }
  }

  clock_model_append(&node->model, local_ticks, hub_ticks);

  if (node->model.count == CLOCK_MODEL_WINDOW_SIZE) {
    struct clock_fit_result fit;

    if (!clock_model_compute_fit(&node->model, &fit)) {
      sync_node_reset(node, "singular_fit");
      return false;
    }

    const double rms_limit = had_valid_model
                                 ? CLOCK_MODEL_TRACK_RMS_LIMIT_TICKS
                                 : CLOCK_MODEL_ACQUIRE_RMS_LIMIT_TICKS;

    const double skew_ppm = (fit.slope - 1.0) * 1000000.0;
    const bool skew_ok = math_abs_double(skew_ppm) <= CLOCK_MODEL_MAX_SKEW_PPM;
    const bool rms_ok = fit.rms_ticks <= rms_limit;

    if (!skew_ok || !rms_ok) {
      const char *reason =
          (!skew_ok && !rms_ok) ? "SKEW_AND_RMS" : (!skew_ok ? "SKEW" : "RMS");

      printk("FIT_REJECT,%s,%u,%s,%llu,%llu,%u,%lld,%lld,%lld\n", node->name,
             pulse_number, reason, (unsigned long long)hub_ticks,
             (unsigned long long)local_ticks, (unsigned int)node->model.count,
             (long long)clock_slope_to_ppb(fit.slope),
             (long long)ticks_to_ns(fit.rms_ticks),
             (long long)ticks_to_ns(rms_limit));

      sync_node_reset(node, had_valid_model ? "tracking_fit_fault"
                                            : "acquisition_fit_fault");
      return false;
    }

    if (had_valid_model) {
      const double old_prediction =
          clock_model_predict(&node->model, local_ticks);
      const double new_prediction = clock_fit_predict(&fit, local_ticks);

      model_step_ticks = new_prediction - old_prediction;

      if (math_abs_double(model_step_ticks) >
          CLOCK_MODEL_MAX_UPDATE_STEP_TICKS) {
        printk("MODEL_STEP_REJECT,%s,%u,%lld,%lld\n", node->name, pulse_number,
               (long long)ticks_to_ns(model_step_ticks),
               (long long)ticks_to_ns(CLOCK_MODEL_MAX_UPDATE_STEP_TICKS));
        sync_node_reset(node, "discontinuous_model_update");
        return false;
      }
    }

    node->model.slope = fit.slope;
    node->model.local_reference = fit.local_reference;
    node->model.hub_at_local_reference = fit.hub_at_local_reference;
    node->model.rms_ticks = fit.rms_ticks;
    node->model.valid = true;
  }

  node->have_previous_pair = true;
  node->previous_pulse_number = pulse_number;
  node->previous_local_ticks = local_ticks;
  node->consecutive_admission_rejections = 0U;

  printk(
      "SYNC,%s,%u,%llu,%llu,%u,%u,%u,%s,%lld,%llu,%lld,%lld,%lld,%lld,%llu\n",
      node->name, pulse_number, (unsigned long long)hub_ticks,
      (unsigned long long)local_ticks, snapshot->status_flags,
      snapshot->record_flags, snapshot->pending_count,
      node->model.valid ? "TRACK" : "ACQUIRE",
      (long long)(node->model.valid ? clock_slope_to_ppb(node->model.slope)
                                    : 0LL),
      (unsigned long long)(node->model.valid ? node->model.local_reference
                                             : 0ULL),
      (long long)(node->model.valid
                      ? math_round_i64(node->model.hub_at_local_reference)
                      : 0LL),
      (long long)(node->model.valid ? ticks_to_ns(node->model.rms_ticks) : 0LL),
      (long long)(node->model.valid ? ticks_to_ns(prefit_residual_ticks) : 0LL),
      (long long)(node->model.valid ? ticks_to_ns(model_step_ticks) : 0LL),
      (unsigned long long)transport_age_ticks);

  return true;
}

/* -------------------------------------------------------------------------- */
/* Hub TIMER2 time and node transport                                  */
/* -------------------------------------------------------------------------- */

static uint32_t hub_timer_phase_ticks(void) {
  return nrfx_timer_capture(&hub_timer, NRF_TIMER_CC_CHANNEL2);
}

/*
 * Convert the periodic, auto-cleared TIMER2 into a monotonically increasing
 * session timestamp.  Interrupts are locked while the pulse count, captured
 * phase and pending COMPARE0 event are inspected.  If the hardware boundary
 * occurred while its ISR was masked, COMPARE0 is pending and the freshly
 * cleared timer phase is in the first half of the new interval.
 */
static uint64_t hub_time_now_ticks(void) {
  const unsigned int key = irq_lock();
  uint32_t pulse_count = (uint32_t)atomic_get(&hub_sync_pulse_count);
  const uint32_t phase = hub_timer_phase_ticks();
  const bool boundary_pending =
      nrf_timer_event_check(hub_timer.p_reg, NRF_TIMER_EVENT_COMPARE0);

  if (boundary_pending && (phase < NODE_SYNC_ACCEPTANCE_WINDOW_TICKS)) {
    pulse_count++;
  }

  irq_unlock(key);

  return ((uint64_t)pulse_count * (uint64_t)HUB_SYNC_PERIOD_TICKS) +
         (uint64_t)phase;
}

static k_timeout_t hub_timeout_from_ticks(uint32_t ticks) {
  const uint64_t microseconds = ((uint64_t)ticks + 15ULL) / 16ULL;

  return K_USEC(microseconds);
}

static void hub_schedule_at_phase(struct k_work_delayable *work,
                                     uint32_t target_phase) {
  const uint32_t now = hub_timer_phase_ticks();

  if (now >= target_phase) {
    (void)k_work_reschedule(work, K_NO_WAIT);
    return;
  }

  (void)k_work_reschedule(work, hub_timeout_from_ticks(target_phase - now));
}

static void node_schedule_next_poll(uint32_t current_phase,
                                     uint8_t pending_count) {
  if (current_phase >= NODE_FINAL_DRAIN_PHASE_TICKS) {
    return;
  }

  if (pending_count > 1U) {
    (void)k_work_reschedule(&node_poll_work, K_NO_WAIT);
    return;
  }

  uint32_t target_phase = current_phase + NODE_POLL_INTERVAL_TICKS;
  if (target_phase > NODE_FINAL_DRAIN_PHASE_TICKS) {
    target_phase = NODE_FINAL_DRAIN_PHASE_TICKS;
  }

  hub_schedule_at_phase(&node_poll_work, target_phase);
}

static void node_rs485_uart_callback(const struct device *device,
                                      struct uart_event *event,
                                      void *user_data) {
  ARG_UNUSED(device);
  ARG_UNUSED(user_data);

  switch (event->type) {
  case UART_TX_DONE:
    /* Release the half-duplex bus at the physical end of transmission. */
    nrf_gpio_pin_clear(NODE_RS485_DE_PIN);
    k_sem_give(&node_rs485_tx_done_sem);
    break;

  case UART_TX_ABORTED:
    nrf_gpio_pin_clear(NODE_RS485_DE_PIN);
    node_rs485_async_error = -EIO;
    k_sem_give(&node_rs485_tx_done_sem);
    break;

  case UART_RX_RDY: {
    const size_t end = event->data.rx.offset + event->data.rx.len;
    if (end > node_rs485_received_length) {
      node_rs485_received_length = end;
    }
    if (node_rs485_received_length >= node_rs485_expected_rx_length) {
      k_sem_give(&node_rs485_rx_done_sem);
    }
    break;
  }

  case UART_RX_STOPPED:
    node_rs485_async_error = -EIO;
    k_sem_give(&node_rs485_rx_done_sem);
    break;

  case UART_RX_DISABLED:
    k_sem_give(&node_rs485_rx_disabled_sem);
    break;

  case UART_RX_BUF_REQUEST:
  case UART_RX_BUF_RELEASED:
  default:
    break;
  }
}

static int node_rs485_init(void) {
  if (!device_is_ready(node_uart)) {
    return -ENODEV;
  }

  nrf_gpio_cfg_output(NODE_RS485_DE_PIN);
  nrf_gpio_pin_clear(NODE_RS485_DE_PIN);

  return uart_callback_set(node_uart, node_rs485_uart_callback, NULL);
}

static uint8_t node_rs485_allocate_sequence(void) {
  const uint8_t sequence = node_rs485_next_bus_sequence;

  node_rs485_next_bus_sequence++;
  if (node_rs485_next_bus_sequence == NODE_RS485_NO_ACK) {
    node_rs485_next_bus_sequence = 0U;
  }

  return sequence;
}

static void node_rs485_stop_rx(void) {
  const int error = uart_rx_disable(node_uart);

  if (error == 0) {
    (void)k_sem_take(&node_rs485_rx_disabled_sem,
                     K_MSEC(NODE_RS485_RX_DISABLE_TIMEOUT_MS));
  }
}

static bool node_rs485_validate_response(const uint8_t *response,
                                          size_t response_length,
                                          uint8_t opcode, uint8_t sequence) {
  if ((response_length < 8U) || (response[0] != NODE_RS485_RESPONSE_MAGIC_0) ||
      (response[1] != NODE_RS485_RESPONSE_MAGIC_1) ||
      (response[2] != NODE_RS485_ADDRESS) ||
      (response[3] != (uint8_t)(opcode | NODE_RS485_RESPONSE_BIT)) ||
      (response[4] != sequence)) {
    return false;
  }

  const uint16_t received_crc = sys_get_le16(&response[response_length - 2U]);
  const uint16_t computed_crc = crc16_ccitt(response, response_length - 2U);

  return received_crc == computed_crc;
}

static int node_rs485_exchange(uint8_t opcode, uint8_t value,
                                uint8_t *response, size_t response_length,
                                uint8_t *sequence_out) {
  if ((response == NULL) || (response_length > NODE_RS485_MAX_RESPONSE_SIZE)) {
    return -EINVAL;
  }

  const uint8_t sequence = node_rs485_allocate_sequence();
  uint8_t request[NODE_RS485_REQUEST_SIZE] = {
      NODE_RS485_REQUEST_MAGIC_0,
      NODE_RS485_REQUEST_MAGIC_1,
      NODE_RS485_ADDRESS,
      opcode,
      sequence,
      value,
      0U,
      0U,
  };

  sys_put_le16(crc16_ccitt(request, NODE_RS485_REQUEST_SIZE - 2U),
               &request[NODE_RS485_REQUEST_SIZE - 2U]);

  int final_error = -ETIMEDOUT;

  k_mutex_lock(&node_rs485_mutex, K_FOREVER);

  for (unsigned int attempt = 0U; attempt < NODE_RS485_RETRY_COUNT;
       ++attempt) {
    k_sem_reset(&node_rs485_tx_done_sem);
    k_sem_reset(&node_rs485_rx_done_sem);
    k_sem_reset(&node_rs485_rx_disabled_sem);

    node_rs485_expected_rx_length = response_length;
    node_rs485_received_length = 0U;
    node_rs485_async_error = 0;
    memset(response, 0, response_length);

    int error = uart_rx_enable(node_uart, response, response_length,
                               NODE_RS485_RX_IDLE_TIMEOUT_US);
    if (error != 0) {
      final_error = error;
      continue;
    }

    /* DE and /RE are tied: high transmits, low receives. */
    nrf_gpio_pin_set(NODE_RS485_DE_PIN);
    k_busy_wait(1U);

    error = uart_tx(node_uart, request, sizeof(request),
                    NODE_RS485_TX_TIMEOUT_US);
    if (error != 0) {
      nrf_gpio_pin_clear(NODE_RS485_DE_PIN);
      node_rs485_stop_rx();
      final_error = error;
      continue;
    }

    error = k_sem_take(&node_rs485_tx_done_sem,
                       K_MSEC(NODE_RS485_TRANSACTION_TIMEOUT_MS));
    if (error != 0) {
      nrf_gpio_pin_clear(NODE_RS485_DE_PIN);
    }

    if ((error != 0) || (node_rs485_async_error != 0)) {
      node_rs485_stop_rx();
      final_error =
          (node_rs485_async_error != 0) ? node_rs485_async_error : -ETIMEDOUT;
      continue;
    }

    error = k_sem_take(&node_rs485_rx_done_sem,
                       K_MSEC(NODE_RS485_TRANSACTION_TIMEOUT_MS));
    const size_t received = node_rs485_received_length;
    const int async_error = node_rs485_async_error;

    node_rs485_stop_rx();

    if ((error != 0) || (async_error != 0) || (received != response_length)) {
      final_error = (async_error != 0) ? async_error : -ETIMEDOUT;
      continue;
    }

    if (!node_rs485_validate_response(response, response_length, opcode,
                                       sequence)) {
      final_error = -EBADMSG;
      continue;
    }

    if (sequence_out != NULL) {
      *sequence_out = sequence;
    }

    final_error = 0;
    break;
  }

  k_mutex_unlock(&node_rs485_mutex);
  return final_error;
}

static int node_write_command(uint8_t command) {
  uint8_t response[NODE_RS485_COMMAND_RESPONSE_SIZE];
  const int error = node_rs485_exchange(NODE_RS485_OPCODE_COMMAND, command,
                                         response, sizeof(response), NULL);

  if (error != 0) {
    return error;
  }

  return (response[5] == 0U) ? 0 : -EIO;
}

static int node_read_frame(struct remote_frame *snapshot) {
  uint8_t response[NODE_RS485_POLL_RESPONSE_SIZE];
  uint8_t response_sequence = 0U;
  const uint8_t ack_sequence = node_rs485_have_poll_ack
                                   ? node_rs485_poll_ack_sequence
                                   : NODE_RS485_NO_ACK;

  const int error =
      node_rs485_exchange(NODE_RS485_OPCODE_POLL, ack_sequence, response,
                           sizeof(response), &response_sequence);

  if (error != 0) {
    return error;
  }

  if (response[5] != REMOTE_FRAME_SIZE) {
    return -EBADMSG;
  }

  if (!remote_frame_decode(&response[6], snapshot)) {
    return -EBADMSG;
  }

  node_rs485_poll_ack_sequence = response_sequence;
  node_rs485_have_poll_ack = true;
  return 0;
}

static void node_fault_history_reset(void) {
  memset(node_timeout_history, 0, sizeof(node_timeout_history));
  node_timeout_history_index = 0U;
  node_timeout_history_count = 0U;
  node_timeout_history_sum = 0U;
  node_consecutive_timeouts = 0U;
  node_consecutive_bad_windows = 0U;
}

static void node_timeout_history_record(bool timed_out) {
  if (node_timeout_history_count < NODE_TIMEOUT_HISTORY_SIZE) {
    node_timeout_history_count++;
  } else {
    node_timeout_history_sum -=
        node_timeout_history[node_timeout_history_index];
  }

  node_timeout_history[node_timeout_history_index] = timed_out ? 1U : 0U;
  node_timeout_history_sum +=
      node_timeout_history[node_timeout_history_index];
  node_timeout_history_index =
      (node_timeout_history_index + 1U) % NODE_TIMEOUT_HISTORY_SIZE;

  if (timed_out) {
    node_consecutive_timeouts++;
  } else {
    node_consecutive_timeouts = 0U;
  }
}

static void node_sync_window_timeout(uint32_t pulse_number) {
  node_timeout_history_record(true);
  printk("WINDOW_TIMEOUT,%s,%u\n", NODE_NODE, pulse_number);

  if (!node_node.model.valid) {
    sync_node_reset(&node_node, "acquisition_timeout");
    return;
  }

  if ((node_consecutive_timeouts >= NODE_MAX_CONSECUTIVE_TIMEOUTS) ||
      (node_timeout_history_sum >= NODE_MAX_TIMEOUTS_IN_HISTORY)) {
    sync_node_reset(&node_node, "timeout_rate_fault");
    node_fault_history_reset();
  }
}

static void node_sync_window_reject(uint32_t pulse_number,
                                     const char *reason) {
  node_timeout_history_record(false);
  node_consecutive_bad_windows++;

  printk("WINDOW_REJECT,%s,%u,%s\n", NODE_NODE, pulse_number, reason);

  if (!node_node.model.valid) {
    sync_node_reset(&node_node, reason);
    return;
  }

  if (node_consecutive_bad_windows >= NODE_MAX_CONSECUTIVE_BAD_WINDOWS) {
    sync_node_reset(&node_node, "repeated_model_independent_fault");
    node_fault_history_reset();
  }
}

static void node_log_event(const struct remote_frame *frame) {
  /* Node firmware does not yet place an event sequence in auxiliary. */
  const uint32_t event_id = (uint32_t)atomic_inc(&node_event_sequence) + 1U;

  stream_log_remote_gpio_event(NODE_NODE, event_id, NODE_TIMER_NOMINAL_HZ,
                               &node_node, NULL, frame);
}

static void ttl_generator_log_event(const struct remote_frame *frame) {
  stream_log_remote_gpio_event(NODE_TTL_GENERATOR, frame->auxiliary,
                               TTL_GENERATOR_EVENT_CLOCK_HZ, &ttl_generator_node,
                               &ttl_generator_state_mutex, frame);
}

static void node_handle_status(const struct remote_frame *snapshot) {
  if ((snapshot->status_flags & REMOTE_STATUS_FIRST_AFTER_RESET) != 0U) {
    if (!node_reset_latch_handled) {
      printk("PORT_RESET,%s,0x%08x\n", NODE_NODE, snapshot->auxiliary);
      sync_node_reset(&node_node, "node_reset");
      node_fault_history_reset();

      if (node_sync_window_state.open) {
        node_sync_window_state.invalid = true;
      }

      node_reset_latch_handled = true;
    }

    if (node_write_command(NODE_COMMAND_ACK_RESET_SEEN) != 0) {
      printk("RS485_COMMAND_ERROR,%s,ACK_RESET\n", NODE_NODE);
    }
  } else {
    node_reset_latch_handled = false;
  }

  if ((snapshot->status_flags & REMOTE_STATUS_CLOCK_FAULT) != 0U) {
    sync_node_reset(&node_node, "node_clock_fault");
    node_fault_history_reset();

    if (node_sync_window_state.open) {
      node_sync_window_state.invalid = true;
    }
  }

  const bool capture_loss =
      (snapshot->status_flags & REMOTE_STATUS_CAPTURE_LOSS_LATCHED) != 0U;
  const bool transport_error =
      (snapshot->status_flags & REMOTE_STATUS_TRANSPORT_ERROR_LATCHED) != 0U;

  if (capture_loss && !node_capture_loss_latch_handled) {
    printk("PORT_CAPTURE_LOSS,%s,%u\n", NODE_NODE,
           snapshot->capture_loss_count);
    sync_node_reset(&node_node, "node_capture_loss");
    node_fault_history_reset();

    if (node_sync_window_state.open) {
      node_sync_window_state.invalid = true;
    }

    node_capture_loss_latch_handled = true;
  } else if (!capture_loss) {
    node_capture_loss_latch_handled = false;
  }

  if (transport_error && !node_transport_error_latch_handled) {
    printk("PORT_TRANSPORT_ERROR_COUNT,%s,%u\n", NODE_NODE,
           snapshot->transport_error_count);
    node_transport_error_latch_handled = true;
  } else if (!transport_error) {
    node_transport_error_latch_handled = false;
  }

  if (capture_loss || transport_error) {
    if (node_write_command(NODE_COMMAND_CLEAR_LATCHED_FLAGS) != 0) {
      printk("RS485_COMMAND_ERROR,%s,CLEAR_FLAGS\n", NODE_NODE);
    }
  }
}

static uint64_t node_scale_phase_ticks(uint32_t phase_ticks,
                                        uint64_t rate_ppm) {
  return (((uint64_t)phase_ticks * rate_ppm) + 999999ULL) / 1000000ULL;
}

static bool node_sync_matches_current_pulse(
    const struct remote_frame *snapshot, uint32_t phase_after_read,
    uint64_t *age_ticks_out, uint64_t *minimum_age_ticks_out,
    uint64_t *maximum_age_ticks_out) {
  const uint64_t age_ticks = snapshot->snapshot_ticks - snapshot->capture_ticks;
  const uint64_t minimum_scaled_age = node_scale_phase_ticks(
      phase_after_read, 1000000ULL - NODE_PAIRING_MAX_RATE_ERROR_PPM);
  const uint64_t lower_allowance =
      NODE_PAIRING_MAX_TRANSPORT_TICKS + NODE_PAIRING_AGE_MARGIN_TICKS;
  const uint64_t minimum_age_ticks =
      (minimum_scaled_age > lower_allowance)
          ? (minimum_scaled_age - lower_allowance)
          : 0ULL;
  uint64_t maximum_age_ticks = node_scale_phase_ticks(
      phase_after_read, 1000000ULL + NODE_PAIRING_MAX_RATE_ERROR_PPM);

  maximum_age_ticks += NODE_PAIRING_AGE_MARGIN_TICKS;
  if (maximum_age_ticks > NODE_LOCAL_ACCEPTANCE_WINDOW_TICKS) {
    maximum_age_ticks = NODE_LOCAL_ACCEPTANCE_WINDOW_TICKS;
  }

  *age_ticks_out = age_ticks;
  *minimum_age_ticks_out = minimum_age_ticks;
  *maximum_age_ticks_out = maximum_age_ticks;

  return (age_ticks >= minimum_age_ticks) && (age_ticks <= maximum_age_ticks);
}

static void node_process_frame(const struct remote_frame *snapshot,
                                uint32_t pulse_before_read,
                                uint32_t pulse_after_read,
                                uint32_t phase_after_read) {
  if ((snapshot->status_flags & REMOTE_STATUS_RECORD_VALID) == 0U) {
    return;
  }

  if (snapshot->record_type == REMOTE_RECORD_COMMAND_ACK) {
    const uint64_t ack_rx_ticks = hub_time_now_ticks();
    if (!host_command.active) {
      printk("COMMAND_ACK_DROP,%u,NO_PENDING\n", snapshot->auxiliary);
      return;
    }

    if (((uint8_t)snapshot->auxiliary & NODE_COMMAND_TOKEN_MASK) !=
        host_command.node_token) {
      host_error_enqueue(host_command.sequence,
                           HOST_STATUS_TOKEN_MISMATCH);
      host_command.active = false;
      return;
    }

    uint64_t node_rx_hub = 0ULL;
    uint64_t node_exec_hub = 0ULL;
    uint32_t node_status = HOST_STATUS_NODE_MODEL_INVALID;
    if (node_node.model.valid) {
      node_rx_hub = (uint64_t)math_round_i64(
          clock_model_predict(&node_node.model, snapshot->capture_ticks));
      node_exec_hub = (uint64_t)math_round_i64(
          clock_model_predict(&node_node.model, snapshot->snapshot_ticks));
      node_status = HOST_STATUS_OK;
    }

    const uint32_t sequence = host_command.sequence;
    const uint64_t hub_rx_ticks = host_command.hub_rx_ticks;
    const uint64_t node_tx_start_ticks = host_command.node_tx_start_ticks;
    const uint64_t done_tx_ticks = hub_time_now_ticks();

    host_command.active = false;
    (void)k_work_cancel_delayable(&host_command_timeout_work);

    stream_log_event(
        NODE_NODE, sequence, EVENT_KIND_COMMAND_RX, snapshot->capture_ticks,
        NODE_TIMER_NOMINAL_HZ,
        node_node.model.valid ? (int64_t)node_rx_hub : -1,
        node_node.model.valid ? EVENT_STATE_TRACK : EVENT_STATE_UNSYNC, 0ULL);

    stream_log_event(
        NODE_NODE, sequence, EVENT_KIND_COMMAND_EXEC, snapshot->snapshot_ticks,
        NODE_TIMER_NOMINAL_HZ,
        node_node.model.valid ? (int64_t)node_exec_hub : -1,
        node_node.model.valid ? EVENT_STATE_TRACK : EVENT_STATE_UNSYNC, 0ULL);

    stream_log_event(NODE_HUB, sequence, EVENT_KIND_COMMAND_ACK_RX,
                     ack_rx_ticks, HUB_TIMER_HZ, (int64_t)ack_rx_ticks,
                     EVENT_STATE_LOCAL, 0ULL);

    stream_log_event(NODE_HUB, sequence, EVENT_KIND_COMMAND_RESULT_QUEUE,
                     done_tx_ticks, HUB_TIMER_HZ, (int64_t)done_tx_ticks,
                     EVENT_STATE_LOCAL, 0ULL);

    /* Put DONE first so diagnostic frames do not inflate Host's RTT. */
    (void)host_notification_enqueue(HOST_MSG_COMMAND_HUB_DONE_TX,
                                      sequence, done_tx_ticks,
                                      HOST_STATUS_OK);
    (void)host_notification_enqueue(HOST_MSG_COMMAND_HUB_RX, sequence,
                                      hub_rx_ticks, HOST_STATUS_OK);
    (void)host_notification_enqueue(HOST_MSG_COMMAND_NODE_TX_START,
                                      sequence, node_tx_start_ticks,
                                      HOST_STATUS_OK);
    (void)host_notification_enqueue(HOST_MSG_COMMAND_NODE_RX, sequence,
                                      node_rx_hub, node_status);
    (void)host_notification_enqueue(HOST_MSG_COMMAND_NODE_EXEC, sequence,
                                      node_exec_hub, node_status);
    (void)host_notification_enqueue(HOST_MSG_COMMAND_HUB_ACK_RX,
                                      sequence, ack_rx_ticks, HOST_STATUS_OK);

    return;
  }

  if (snapshot->record_type == REMOTE_RECORD_EVENT) {
    node_log_event(snapshot);
    return;
  }

  if (snapshot->record_type != REMOTE_RECORD_SYNC) {
    if (node_sync_window_state.open) {
      node_sync_window_state.invalid = true;
    }

    printk("UNKNOWN_RECORD_TYPE,%s,%u\n", NODE_NODE, snapshot->record_type);
    return;
  }

  if ((snapshot->record_flags & REMOTE_RECORD_FLAG_HW_OVERCAPTURE) != 0U) {
    if (node_sync_window_state.open) {
      node_sync_window_state.invalid = true;
    }

    printk("SYNC_OVERCAPTURE_DROP,%s,%u,%llu\n", NODE_NODE, pulse_after_read,
           (unsigned long long)snapshot->capture_ticks);
    return;
  }

  if (pulse_before_read != pulse_after_read) {
    if (node_sync_window_state.open &&
        (node_sync_window_state.pulse_number == pulse_after_read)) {
      node_sync_window_state.invalid = true;
    }

    printk("SYNC_BOUNDARY_DROP,%s,%u,%u,%llu\n", NODE_NODE, pulse_before_read,
           pulse_after_read, (unsigned long long)snapshot->capture_ticks);
    return;
  }

  const bool inside_current_window =
      node_sync_window_state.open &&
      (node_sync_window_state.pulse_number == pulse_after_read) &&
      (phase_after_read < NODE_SYNC_ACCEPTANCE_WINDOW_TICKS);

  if (!inside_current_window) {
    printk("DEAD_ZONE_SYNC_DROP,%s,%u,%u,%llu\n", NODE_NODE, pulse_after_read,
           phase_after_read, (unsigned long long)snapshot->capture_ticks);
    return;
  }

  uint64_t sync_age_ticks;
  uint64_t minimum_sync_age_ticks;
  uint64_t maximum_sync_age_ticks;

  if (!node_sync_matches_current_pulse(
          snapshot, phase_after_read, &sync_age_ticks, &minimum_sync_age_ticks,
          &maximum_sync_age_ticks)) {
    printk("OUT_OF_PHASE_SYNC_DROP,%s,%u,%llu,%llu,%llu,%llu\n", NODE_NODE,
           node_sync_window_state.pulse_number,
           (unsigned long long)snapshot->capture_ticks,
           (unsigned long long)sync_age_ticks,
           (unsigned long long)minimum_sync_age_ticks,
           (unsigned long long)maximum_sync_age_ticks);
    return;
  }

  node_sync_window_state.sync_reports_seen++;

  if (node_sync_window_state.candidate_valid ||
      node_sync_window_state.invalid) {
    node_sync_window_state.invalid = true;

    printk("DUPLICATE_SYNC_IN_WINDOW,%s,%u,%u,%llu\n", NODE_NODE,
           node_sync_window_state.pulse_number,
           node_sync_window_state.sync_reports_seen,
           (unsigned long long)snapshot->capture_ticks);
    return;
  }

  node_sync_window_state.candidate = *snapshot;
  node_sync_window_state.candidate_valid = true;
}

static void host_request_work_handler(struct k_work *work) {
  ARG_UNUSED(work);
  struct host_request request;

  while (k_msgq_get(&host_request_queue, &request, K_NO_WAIT) == 0) {
    if (request.type == HOST_MSG_CLOCK_SYNC_REQUEST) {
      const uint64_t t2 = request.hub_rx_ticks;

      const uint64_t t3 = hub_time_now_ticks();

      const uint64_t delta = t3 - t2;

      const uint32_t processing_ticks =
          delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta;

      const int notify_error = host_notification_enqueue(
          HOST_MSG_CLOCK_REPLY, request.sequence, t2, processing_ticks);

      stream_log_event(NODE_HUB, request.sequence,
                       EVENT_KIND_CLOCK_SYNC_REPLY_QUEUE, t3, HUB_TIMER_HZ,
                       (int64_t)t3, EVENT_STATE_LOCAL, 0ULL);

      if (notify_error != 0) {
        stream_log_fault(NODE_HOST, "CLOCK_REPLY_QUEUE", notify_error,
                         request.sequence);
      }

      continue;
    }

    if (host_command.active) {
      host_error_enqueue(request.sequence, HOST_STATUS_BUSY);
      continue;
    }

    host_command.active = true;
    host_command.sequence = request.sequence;
    host_command.node_token =
        (uint8_t)(request.sequence & NODE_COMMAND_TOKEN_MASK);
    host_command.deadline_ticks =
        request.hub_rx_ticks + HOST_COMMAND_TIMEOUT_TICKS;
    host_command.hub_rx_ticks = request.hub_rx_ticks;
    host_command.node_tx_start_ticks = hub_time_now_ticks();

    const uint8_t command =
        NODE_COMMAND_DO_NOW_MASK | host_command.node_token;

    const int error = node_write_command(command);

    stream_log_event(NODE_HUB, request.sequence, EVENT_KIND_COMMAND_FORWARD,
                     host_command.node_tx_start_ticks, HUB_TIMER_HZ,
                     (int64_t)host_command.node_tx_start_ticks,
                     EVENT_STATE_LOCAL, 0ULL);

    if (error != 0) {
      stream_log_fault(NODE_NODE, "RS485_COMMAND_WRITE", error,
                       request.sequence);
      host_error_enqueue(request.sequence,
                           HOST_STATUS_NODE_TRANSPORT_FAILED);

      host_command.active = false;
      continue;
    }

    (void)k_work_reschedule(&host_command_timeout_work, K_MSEC(10));
  }
}

static void host_command_timeout_work_handler(struct k_work *work) {
  ARG_UNUSED(work);

  if (!host_command.active) {
    return;
  }

  const uint64_t now = hub_time_now_ticks();

  if (now < host_command.deadline_ticks) {
    /*
     * This work item is only a timeout watchdog.
     * It must never read from node.
     */
    (void)k_work_reschedule(&host_command_timeout_work, K_MSEC(10));

    return;
  }

  stream_log_fault(NODE_HOST, "COMMAND_TIMEOUT", -ETIMEDOUT,
                   host_command.sequence);

  host_error_enqueue(host_command.sequence, HOST_STATUS_TIMEOUT);

  host_command.active = false;
}

/* -------------------------------------------------------------------------- */
/* nRF52 SoftDevice-controller time mirror and TIMER2 bridge                  */
/* -------------------------------------------------------------------------- */

#define CONTROLLER_RTC_TICK_FEMTOSECONDS 30517578125ULL
#define CONTROLLER_RTC_OVERFLOW_US 512000000ULL
#define CONTROLLER_BRIDGE_RMS_LIMIT_TICKS 1600.0 /* 100 us */
#define CONTROLLER_BRIDGE_MAX_SKEW_PPM 2000.0
#define CONTROLLER_BRIDGE_MAX_SAMPLE_SPAN_TICKS 32000ULL /* 2 ms */

static uint64_t ttl_generator_rtc_ticks_to_us(uint32_t rtc_ticks) {
  return ((uint64_t)rtc_ticks * CONTROLLER_RTC_TICK_FEMTOSECONDS) /
         1000000000ULL;
}

static int32_t ttl_generator_rtc_offset_measure(void) {
  const uint32_t controller_ticks = nrf_rtc_counter_get(NRF_RTC0);
  const uint32_t application_ticks =
      nrf_rtc_counter_get(ttl_generator_controller_rtc.p_reg);

  return (int32_t)(controller_ticks - application_ticks);
}

static void ttl_generator_rtc_event_handler(nrf_rtc_event_t event_type,
                                        void *context) {
  ARG_UNUSED(context);

  if (event_type == NRF_RTC_EVENT_OVERFLOW) {
    ttl_generator_controller_rtc_overflows++;
  }
}

static int ttl_generator_controller_clock_init(void) {
  const nrfx_rtc_config_t config = NRFX_RTC_DEFAULT_CONFIG;
  int error = nrfx_rtc_init(&ttl_generator_controller_rtc, &config,
                            ttl_generator_rtc_event_handler);

  if (error != 0) {
    return error;
  }

  IRQ_CONNECT(NRFX_IRQ_NUMBER_GET(NRF_RTC_INST_GET(2)), IRQ_PRIO_LOWEST,
              nrfx_rtc_irq_handler, &ttl_generator_controller_rtc, 0);

  nrfx_rtc_overflow_enable(&ttl_generator_controller_rtc, true);
  nrfx_rtc_tick_enable(&ttl_generator_controller_rtc, false);
  nrfx_rtc_enable(&ttl_generator_controller_rtc);

  for (unsigned int attempt = 0U; attempt < 10U; ++attempt) {
    const int32_t first = ttl_generator_rtc_offset_measure();
    k_busy_wait(15U);
    const int32_t second = ttl_generator_rtc_offset_measure();

    if (first == second) {
      ttl_generator_controller_to_app_rtc_offset_ticks = (uint32_t)first;
      printk("CONTROLLER_CLOCK_READY,%s,%u\n", NODE_HUB,
             ttl_generator_controller_to_app_rtc_offset_ticks);
      return 0;
    }
  }

  return -EIO;
}

static uint64_t ttl_generator_controller_time_us(void) {
  uint32_t captured_overflows;
  uint32_t captured_ticks;

  while (true) {
    captured_overflows = ttl_generator_controller_rtc_overflows;
    barrier_isync_fence_full();
    captured_ticks = nrf_rtc_counter_get(ttl_generator_controller_rtc.p_reg);
    barrier_isync_fence_full();

    if (captured_overflows == ttl_generator_controller_rtc_overflows) {
      break;
    }
  }

  return ttl_generator_rtc_ticks_to_us(captured_ticks) +
         ((uint64_t)captured_overflows * CONTROLLER_RTC_OVERFLOW_US) +
         ttl_generator_rtc_ticks_to_us(
             ttl_generator_controller_to_app_rtc_offset_ticks);
}

static void ttl_generator_controller_bridge_sample(void) {
  const uint64_t hub_before = hub_time_now_ticks();
  const uint64_t controller_ticks =
      ttl_generator_controller_time_us() * TTL_GENERATOR_CONTROLLER_TICKS_PER_US;
  const uint64_t hub_after = hub_time_now_ticks();

  if ((hub_after < hub_before) ||
      ((hub_after - hub_before) > CONTROLLER_BRIDGE_MAX_SAMPLE_SPAN_TICKS)) {
    printk("BRIDGE_SAMPLE_DROP,%s,%llu\n", NODE_HUB,
           (unsigned long long)(hub_after - hub_before));
    return;
  }

  const uint64_t hub_midpoint = hub_before + ((hub_after - hub_before) / 2ULL);

  clock_model_append(&ttl_generator_controller_bridge, controller_ticks,
                     hub_midpoint);

  if (ttl_generator_controller_bridge.count < CLOCK_MODEL_WINDOW_SIZE) {
    return;
  }

  struct clock_fit_result fit;
  if (!clock_model_compute_fit(&ttl_generator_controller_bridge, &fit)) {
    printk("BRIDGE_FIT_REJECT,%s,SINGULAR\n", NODE_HUB);
    return;
  }

  const double skew_ppm = (fit.slope - 1.0) * 1000000.0;

  if ((math_abs_double(skew_ppm) > CONTROLLER_BRIDGE_MAX_SKEW_PPM) ||
      (fit.rms_ticks > CONTROLLER_BRIDGE_RMS_LIMIT_TICKS)) {
    printk("BRIDGE_FIT_REJECT,%s,%lld,%lld\n", NODE_HUB,
           (long long)clock_slope_to_ppb(fit.slope),
           (long long)ticks_to_ns(fit.rms_ticks));
    return;
  }

  ttl_generator_controller_bridge.slope = fit.slope;
  ttl_generator_controller_bridge.local_reference = fit.local_reference;
  ttl_generator_controller_bridge.hub_at_local_reference =
      fit.hub_at_local_reference;
  ttl_generator_controller_bridge.rms_ticks = fit.rms_ticks;
  ttl_generator_controller_bridge.valid = true;

  if (!ttl_generator_controller_bridge_announced) {
    ttl_generator_controller_bridge_announced = true;
    printk("BRIDGE_READY,%s,%lld,%lld\n", NODE_HUB,
           (long long)clock_slope_to_ppb(ttl_generator_controller_bridge.slope),
           (long long)ticks_to_ns(ttl_generator_controller_bridge.rms_ticks));
  }
}

static bool ttl_generator_anchor_to_hub_ticks(uint64_t controller_ticks,
                                             uint64_t *hub_ticks_out) {
  if (!ttl_generator_controller_bridge.valid) {
    return false;
  }

  const double prediction =
      clock_model_predict(&ttl_generator_controller_bridge, controller_ticks);

  if (prediction < 0.0) {
    return false;
  }

  *hub_ticks_out = (uint64_t)math_round_i64(prediction);
  return true;
}

/* -------------------------------------------------------------------------- */
/* Bluetooth central and anchor matching                                      */
/* -------------------------------------------------------------------------- */

static const struct bt_uuid *const ttl_generator_sync_service_uuid =
    BT_UUID_HUB_SYNC_SERVICE;
static const struct bt_uuid *const ttl_generator_ttl_control_uuid =
    BT_UUID_HUB_TTL_CONTROL;
static const struct bt_uuid *const ttl_generator_anchor_report_uuid =
    BT_UUID_HUB_ANCHOR_REPORT;

static uint32_t ttl_generator_last_capture_loss_count;
static uint32_t ttl_generator_last_transport_error_count;
static bool ttl_generator_waiting_for_bridge_announced;

static void ttl_generator_anchor_cache_reset(void) {
  const k_spinlock_key_t key = k_spin_lock(&ttl_generator_cache_lock);

  memset(ttl_generator_anchor_cache, 0, sizeof(ttl_generator_anchor_cache));

  k_spin_unlock(&ttl_generator_cache_lock, key);
  k_msgq_purge(&ttl_generator_pair_queue);

  ttl_generator_have_unwrapped_counter = false;
  ttl_generator_previous_event_counter = 0U;
  ttl_generator_unwrapped_event_counter = 0U;
  ttl_generator_waiting_for_bridge_announced = false;
}

static void ttl_generator_pair_enqueue(const struct ttl_generator_pair *pair) {
  if (k_msgq_put(&ttl_generator_pair_queue, pair, K_NO_WAIT) != 0) {
    printk("TTL_GENERATOR_PAIR_DROP,%s,%u,QUEUE_FULL\n", NODE_TTL_GENERATOR,
           pair->event_counter);
    return;
  }

  (void)k_work_submit(&ttl_generator_pair_work);
}

static void ttl_generator_cache_store_central(uint16_t event_counter,
                                          uint64_t central_ticks,
                                          uint32_t generation) {
  struct ttl_generator_pair pair;
  bool pair_ready = false;

  const k_spinlock_key_t key = k_spin_lock(&ttl_generator_cache_lock);
  struct ttl_generator_anchor_slot *const slot =
      &ttl_generator_anchor_cache[event_counter &
                              (TTL_GENERATOR_ANCHOR_CACHE_SIZE - 1U)];

  if ((slot->central_valid || slot->peripheral_valid) &&
      ((slot->event_counter != event_counter) ||
       (slot->generation != generation))) {
    memset(slot, 0, sizeof(*slot));
  }

  slot->event_counter = event_counter;
  slot->generation = generation;
  slot->central_controller_ticks = central_ticks;
  slot->central_valid = true;

  if (slot->peripheral_valid) {
    pair.event_counter = event_counter;
    pair.generation = generation;
    pair.central_controller_ticks = slot->central_controller_ticks;
    pair.peripheral = slot->peripheral;
    memset(slot, 0, sizeof(*slot));
    pair_ready = true;
  }

  k_spin_unlock(&ttl_generator_cache_lock, key);

  if (pair_ready) {
    ttl_generator_pair_enqueue(&pair);
  }
}

static void ttl_generator_cache_store_remote(uint16_t event_counter,
                                         const struct remote_frame *snapshot,
                                         uint32_t generation) {
  struct ttl_generator_pair pair;
  bool pair_ready = false;

  const k_spinlock_key_t key = k_spin_lock(&ttl_generator_cache_lock);
  struct ttl_generator_anchor_slot *const slot =
      &ttl_generator_anchor_cache[event_counter &
                              (TTL_GENERATOR_ANCHOR_CACHE_SIZE - 1U)];

  if ((slot->central_valid || slot->peripheral_valid) &&
      ((slot->event_counter != event_counter) ||
       (slot->generation != generation))) {
    memset(slot, 0, sizeof(*slot));
  }

  slot->event_counter = event_counter;
  slot->generation = generation;
  slot->peripheral = *snapshot;
  slot->peripheral_valid = true;

  if (slot->central_valid) {
    pair.event_counter = event_counter;
    pair.generation = generation;
    pair.central_controller_ticks = slot->central_controller_ticks;
    pair.peripheral = slot->peripheral;
    memset(slot, 0, sizeof(*slot));
    pair_ready = true;
  }

  k_spin_unlock(&ttl_generator_cache_lock, key);

  if (pair_ready) {
    ttl_generator_pair_enqueue(&pair);
  }
}

static void ttl_generator_handle_status(const struct remote_frame *snapshot) {
  if ((snapshot->status_flags & REMOTE_STATUS_FIRST_AFTER_RESET) != 0U) {
    sync_node_reset(&ttl_generator_node, "ttl_generator_peripheral_reset");
  }

  if ((snapshot->status_flags & REMOTE_STATUS_CLOCK_FAULT) != 0U) {
    sync_node_reset(&ttl_generator_node, "ttl_generator_peripheral_clock_fault");
  }

  if (snapshot->capture_loss_count != ttl_generator_last_capture_loss_count) {
    ttl_generator_last_capture_loss_count = snapshot->capture_loss_count;
    printk("PORT_CAPTURE_LOSS,%s,%u\n", NODE_TTL_GENERATOR,
           snapshot->capture_loss_count);
  }

  if (snapshot->transport_error_count != ttl_generator_last_transport_error_count) {
    ttl_generator_last_transport_error_count = snapshot->transport_error_count;
    printk("PORT_TRANSPORT_ERROR_COUNT,%s,%u\n", NODE_TTL_GENERATOR,
           snapshot->transport_error_count);
  }
}

static void ttl_generator_pair_work_handler(struct k_work *work) {
  ARG_UNUSED(work);

  struct ttl_generator_pair pair;

  while (k_msgq_get(&ttl_generator_pair_queue, &pair, K_NO_WAIT) == 0) {
    uint64_t central_hub_ticks;

    if (!ttl_generator_anchor_to_hub_ticks(pair.central_controller_ticks,
                                          &central_hub_ticks)) {
      if (!ttl_generator_waiting_for_bridge_announced) {
        ttl_generator_waiting_for_bridge_announced = true;
        printk("TTL_GENERATOR_WAIT,%s,CONTROLLER_BRIDGE_ACQUIRE\n", NODE_TTL_GENERATOR);
      }
      continue;
    }

    ttl_generator_waiting_for_bridge_announced = false;

    k_mutex_lock(&ttl_generator_state_mutex, K_FOREVER);

    if ((atomic_get(&ttl_generator_connection_active) == 0) ||
        (pair.generation !=
         (uint32_t)atomic_get(&ttl_generator_connection_generation))) {
      k_mutex_unlock(&ttl_generator_state_mutex);
      printk("TTL_GENERATOR_PAIR_DROP,%s,%u,STALE_CONNECTION\n", NODE_TTL_GENERATOR,
             pair.event_counter);
      continue;
    }

    ttl_generator_handle_status(&pair.peripheral);

    uint32_t unwrapped_counter;

    if (!ttl_generator_have_unwrapped_counter) {
      ttl_generator_unwrapped_event_counter = (uint32_t)pair.event_counter;
      ttl_generator_have_unwrapped_counter = true;
    } else {
      const uint16_t event_delta =
          (uint16_t)(pair.event_counter - ttl_generator_previous_event_counter);

      if (event_delta == 0U) {
        k_mutex_unlock(&ttl_generator_state_mutex);
        printk("TTL_GENERATOR_PAIR_DROP,%s,%u,DUPLICATE\n", NODE_TTL_GENERATOR,
               pair.event_counter);
        continue;
      }

      if (event_delta > 0x8000U) {
        k_mutex_unlock(&ttl_generator_state_mutex);
        printk("TTL_GENERATOR_PAIR_DROP,%s,%u,OUT_OF_ORDER\n", NODE_TTL_GENERATOR,
               pair.event_counter);
        continue;
      }

      ttl_generator_unwrapped_event_counter += (uint32_t)event_delta;
    }

    ttl_generator_previous_event_counter = pair.event_counter;
    unwrapped_counter = ttl_generator_unwrapped_event_counter;

    (void)sync_node_process_pair(&ttl_generator_node, unwrapped_counter,
                                 central_hub_ticks, &pair.peripheral);

    k_mutex_unlock(&ttl_generator_state_mutex);
  }
}

static bool ttl_generator_model_inverse(uint64_t target_hub_ticks,
                                    uint64_t *local_ticks_out) {
  bool valid = false;

  k_mutex_lock(&ttl_generator_state_mutex, K_FOREVER);

  if (ttl_generator_node.model.valid && (ttl_generator_node.model.slope > 0.0)) {
    const double local = (double)ttl_generator_node.model.local_reference +
                         ((double)target_hub_ticks -
                          ttl_generator_node.model.hub_at_local_reference) /
                             ttl_generator_node.model.slope;

    if ((local >= 0.0) && (local <= (double)UINT64_MAX)) {
      uint64_t ticks = (uint64_t)math_round_i64(local);

      /* GRTC has 1 us resolution, represented as 16 nominal ticks. */
      ticks = ((ticks + 8ULL) / 16ULL) * 16ULL;
      *local_ticks_out = ticks;
      valid = true;
    }
  }

  k_mutex_unlock(&ttl_generator_state_mutex);
  return valid;
}

static int64_t ttl_generator_local_to_hub(uint64_t local_ticks, bool *valid_out) {
  int64_t converted = -1;
  bool valid = false;

  k_mutex_lock(&ttl_generator_state_mutex, K_FOREVER);

  if (ttl_generator_node.model.valid) {
    converted =
        math_round_i64(clock_model_predict(&ttl_generator_node.model, local_ticks));
    valid = converted >= 0;
  }

  k_mutex_unlock(&ttl_generator_state_mutex);
  *valid_out = valid;
  return converted;
}

static void ttl_generator_ttl_generated(const struct remote_frame *frame) {
  const uint32_t sequence = frame->auxiliary;
  bool model_valid = false;
  const int64_t generated_hub_ticks =
      ttl_generator_local_to_hub(frame->capture_ticks, &model_valid);

  const k_spinlock_key_t key = k_spin_lock(&ttl_test_lock);

  if (ttl_test.pending && (ttl_test.sequence == sequence)) {
    ttl_test.generated_seen = true;
    ttl_test.generated_local_ticks = frame->capture_ticks;
    ttl_test.generated_hub_ticks = model_valid ? generated_hub_ticks : -1;
  }

  k_spin_unlock(&ttl_test_lock, key);
  (void)k_work_submit(&ttl_result_work);
}

static uint8_t
ttl_generator_notification_callback(struct bt_conn *conn,
                                struct bt_gatt_subscribe_params *params,
                                const void *data, uint16_t length) {
  ARG_UNUSED(conn);

  if (data == NULL) {
    params->value_handle = 0U;

    printk("TTL_GENERATOR_SUBSCRIPTION,%s,ENDED\n", NODE_TTL_GENERATOR);

    return BT_GATT_ITER_STOP;
  }

  if (length != REMOTE_FRAME_SIZE) {
    printk("TTL_GENERATOR_FRAME_DROP,%s,LENGTH,%u\n", NODE_TTL_GENERATOR, length);

    return BT_GATT_ITER_CONTINUE;
  }

  struct remote_frame snapshot;

  if (!remote_frame_decode(data, &snapshot)) {
    printk("TTL_GENERATOR_FRAME_DROP,%s,FORMAT\n", NODE_TTL_GENERATOR);

    return BT_GATT_ITER_CONTINUE;
  }

  if ((snapshot.status_flags & REMOTE_STATUS_RECORD_VALID) == 0U) {
    printk("TTL_GENERATOR_FRAME_DROP,%s,INVALID,%u,0x%02x\n", NODE_TTL_GENERATOR,
           snapshot.record_type, snapshot.status_flags);

    return BT_GATT_ITER_CONTINUE;
  }

  if (atomic_get(&ttl_generator_connection_active) == 0) {
    return BT_GATT_ITER_CONTINUE;
  }

  if (snapshot.record_type == REMOTE_RECORD_TTL_GENERATED) {
    ttl_generator_ttl_generated(&snapshot);
    return BT_GATT_ITER_CONTINUE;
  }

  /*
   * TTL generator external GPIO event.
   *
   * It already contains a TTL generator-local timestamp and must not
   * enter the Bluetooth connection-anchor matching cache.
   */
  if (snapshot.record_type == REMOTE_RECORD_EVENT) {
    ttl_generator_log_event(&snapshot);

    return BT_GATT_ITER_CONTINUE;
  }

  /*
   * Only anchor records continue into the event-counter matcher.
   */
  if (snapshot.record_type != REMOTE_RECORD_SYNC) {
    printk("TTL_GENERATOR_FRAME_DROP,%s,TYPE,%u,0x%02x\n", NODE_TTL_GENERATOR,
           snapshot.record_type, snapshot.status_flags);

    return BT_GATT_ITER_CONTINUE;
  }

  const uint16_t event_counter = (uint16_t)(snapshot.auxiliary & 0xFFFFU);

  const uint32_t generation =
      (uint32_t)atomic_get(&ttl_generator_connection_generation);

  ttl_generator_cache_store_remote(event_counter, &snapshot, generation);

  return BT_GATT_ITER_CONTINUE;
}

static int ttl_generator_subscribe(struct bt_conn *conn, uint16_t ccc_handle) {
  memset(&ttl_generator_client.subscribe, 0, sizeof(ttl_generator_client.subscribe));

  ttl_generator_client.subscribe.notify = ttl_generator_notification_callback;
  ttl_generator_client.subscribe.value = BT_GATT_CCC_NOTIFY;
  ttl_generator_client.subscribe.value_handle = ttl_generator_client.value_handle;
  ttl_generator_client.subscribe.ccc_handle = ccc_handle;

  const int error = bt_gatt_subscribe(conn, &ttl_generator_client.subscribe);

  if ((error != 0) && (error != -EALREADY)) {
    printk("TTL_GENERATOR_SUBSCRIBE_ERROR,%s,%d\n", NODE_TTL_GENERATOR, error);
    return error;
  }

  printk("TTL_GENERATOR_SUBSCRIBED,%s,%u,%u,ttl_control=%u\n", NODE_TTL_GENERATOR,
         ttl_generator_client.value_handle, ccc_handle,
         ttl_generator_client.ttl_control_handle);
  (void)k_work_reschedule(&ttl_schedule_work, K_MSEC(TTL_TEST_RETRY_MS));
  return 0;
}

static uint8_t
ttl_generator_discovery_callback(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params) {
  if (attr == NULL) {
    printk("TTL_GENERATOR_DISCOVERY_ERROR,%s,%u,NOT_FOUND\n", NODE_TTL_GENERATOR,
           (unsigned int)ttl_generator_client.discovery_stage);
    memset(params, 0, sizeof(*params));
    return BT_GATT_ITER_STOP;
  }

  int error;

  switch (ttl_generator_client.discovery_stage) {
  case TTL_GENERATOR_DISCOVERY_SERVICE: {
    const struct bt_gatt_service_val *service = attr->user_data;

    ttl_generator_client.service_end_handle = service->end_handle;
    ttl_generator_client.discovery_stage = TTL_GENERATOR_DISCOVERY_TTL_CHARACTERISTIC;

    params->uuid = ttl_generator_ttl_control_uuid;
    params->start_handle = attr->handle + 1U;
    params->end_handle = ttl_generator_client.service_end_handle;
    params->type = BT_GATT_DISCOVER_CHARACTERISTIC;

    error = bt_gatt_discover(conn, params);
    if (error != 0) {
      printk("TTL_GENERATOR_DISCOVERY_ERROR,%s,CHAR,%d\n", NODE_TTL_GENERATOR, error);
    }
    return BT_GATT_ITER_STOP;
  }

  case TTL_GENERATOR_DISCOVERY_TTL_CHARACTERISTIC: {
    const struct bt_gatt_chrc *characteristic = attr->user_data;

    ttl_generator_client.ttl_control_handle = characteristic->value_handle;
    ttl_generator_client.discovery_stage =
        TTL_GENERATOR_DISCOVERY_REPORT_CHARACTERISTIC;

    params->uuid = ttl_generator_anchor_report_uuid;
    params->start_handle = characteristic->value_handle + 1U;
    params->end_handle = ttl_generator_client.service_end_handle;
    params->type = BT_GATT_DISCOVER_CHARACTERISTIC;

    error = bt_gatt_discover(conn, params);
    if (error != 0) {
      printk("TTL_GENERATOR_DISCOVERY_ERROR,%s,REPORT_CHAR,%d\n", NODE_TTL_GENERATOR,
             error);
    }
    return BT_GATT_ITER_STOP;
  }

  case TTL_GENERATOR_DISCOVERY_REPORT_CHARACTERISTIC: {
    const struct bt_gatt_chrc *characteristic = attr->user_data;

    ttl_generator_client.value_handle = characteristic->value_handle;
    ttl_generator_client.discovery_stage = TTL_GENERATOR_DISCOVERY_CCC;

    params->uuid = BT_UUID_GATT_CCC;
    params->start_handle = characteristic->value_handle + 1U;
    params->end_handle = ttl_generator_client.service_end_handle;
    params->type = BT_GATT_DISCOVER_DESCRIPTOR;

    error = bt_gatt_discover(conn, params);
    if (error != 0) {
      printk("TTL_GENERATOR_DISCOVERY_ERROR,%s,CCC,%d\n", NODE_TTL_GENERATOR, error);
    }
    return BT_GATT_ITER_STOP;
  }

  case TTL_GENERATOR_DISCOVERY_CCC:
    memset(params, 0, sizeof(*params));
    ttl_generator_client.discovery_stage = TTL_GENERATOR_DISCOVERY_NONE;
    (void)ttl_generator_subscribe(conn, attr->handle);
    return BT_GATT_ITER_STOP;

  default:
    return BT_GATT_ITER_STOP;
  }
}

static int ttl_generator_discovery_start(struct bt_conn *conn) {
  memset(&ttl_generator_client.discover, 0, sizeof(ttl_generator_client.discover));

  ttl_generator_client.discovery_stage = TTL_GENERATOR_DISCOVERY_SERVICE;
  ttl_generator_client.discover.uuid = ttl_generator_sync_service_uuid;
  ttl_generator_client.discover.func = ttl_generator_discovery_callback;
  ttl_generator_client.discover.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
  ttl_generator_client.discover.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
  ttl_generator_client.discover.type = BT_GATT_DISCOVER_PRIMARY;

  const int error = bt_gatt_discover(conn, &ttl_generator_client.discover);

  if (error != 0) {
    printk("TTL_GENERATOR_DISCOVERY_ERROR,%s,SERVICE,%d\n", NODE_TTL_GENERATOR, error);
  }

  return error;
}

static void
ttl_generator_mtu_exchange_callback(struct bt_conn *conn, uint8_t error,
                                struct bt_gatt_exchange_params *params) {
  ARG_UNUSED(params);

  printk("TTL_GENERATOR_MTU,%s,%u,%u\n", NODE_TTL_GENERATOR, error,
         bt_gatt_get_mtu(conn));

  if ((error != 0U) || (bt_gatt_get_mtu(conn) < (REMOTE_FRAME_SIZE + 3U))) {
    printk("TTL_GENERATOR_MTU_ERROR,%s,%u\n", NODE_TTL_GENERATOR,
           bt_gatt_get_mtu(conn));
    (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    return;
  }

  (void)ttl_generator_discovery_start(conn);
}

static bool ttl_generator_advertised_name_matches(struct bt_data *data,
                                              void *user_data) {
  bool *const match = user_data;

  if ((data->type != BT_DATA_NAME_COMPLETE) &&
      (data->type != BT_DATA_NAME_SHORTENED)) {
    return true;
  }

  const size_t expected_length = strlen(TTL_GENERATOR_ADVERTISED_NAME);

  if ((data->data_len == expected_length) &&
      (memcmp(data->data, TTL_GENERATOR_ADVERTISED_NAME, expected_length) == 0)) {
    *match = true;
  }

  return false;
}

static int ttl_generator_scan_start(void);

static void ttl_generator_device_found(const bt_addr_le_t *address, int8_t rssi,
                                   uint8_t type,
                                   struct net_buf_simple *advertising_data) {
  ARG_UNUSED(rssi);

  if ((type != BT_GAP_ADV_TYPE_ADV_IND) &&
      (type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND)) {
    return;
  }

  if ((atomic_get(&ttl_generator_connection_active) != 0) ||
      (atomic_get(&ttl_generator_connecting) != 0)) {
    return;
  }

  bool name_matches = false;
  bt_data_parse(advertising_data, ttl_generator_advertised_name_matches,
                &name_matches);

  if (!name_matches || !atomic_cas(&ttl_generator_connecting, 0, 1)) {
    return;
  }

  char address_string[BT_ADDR_LE_STR_LEN];
  bt_addr_le_to_str(address, address_string, sizeof(address_string));

  int error = bt_le_scan_stop();
  if ((error != 0) && (error != -EALREADY)) {
    printk("TTL_GENERATOR_SCAN_ERROR,%s,STOP,%d\n", NODE_TTL_GENERATOR, error);
  }

  struct bt_conn *new_connection = NULL;
  error = bt_conn_le_create(address, BT_CONN_LE_CREATE_CONN,
                            BT_LE_CONN_PARAM(8, 8, 0, 200), &new_connection);

  if (error != 0) {
    printk("TTL_GENERATOR_CONNECT_ERROR,%s,%s,%d\n", NODE_TTL_GENERATOR, address_string,
           error);
    atomic_clear(&ttl_generator_connecting);
    (void)k_work_submit(&ttl_generator_scan_restart_work);
    return;
  }

  if (new_connection != NULL) {
    bt_conn_unref(new_connection);
  }

  printk("TTL_GENERATOR_CONNECTING,%s,%s\n", NODE_TTL_GENERATOR, address_string);
}

static int ttl_generator_scan_start(void) {
  if ((atomic_get(&ttl_generator_connection_active) != 0) ||
      (atomic_get(&ttl_generator_connecting) != 0)) {
    return 0;
  }

  const int error =
      bt_le_scan_start(BT_LE_SCAN_ACTIVE_CONTINUOUS, ttl_generator_device_found);

  if ((error != 0) && (error != -EALREADY)) {
    printk("TTL_GENERATOR_SCAN_ERROR,%s,START,%d\n", NODE_TTL_GENERATOR, error);
    return error;
  }

  stream_log_link(NODE_TTL_GENERATOR, "BLE", "central", "SCANNING",
                  TTL_GENERATOR_ADVERTISED_NAME, 0U, 0U);
  return 0;
}

static void ttl_generator_scan_restart_work_handler(struct k_work *work) {
  ARG_UNUSED(work);
  (void)ttl_generator_scan_start();
}

static void bluetooth_connected(struct bt_conn *conn, uint8_t error) {
  char address[BT_ADDR_LE_STR_LEN];
  bt_addr_le_to_str(bt_conn_get_dst(conn), address, sizeof(address));

  if (error != 0U) {
    atomic_clear(&ttl_generator_connecting);
    printk("TTL_GENERATOR_CONNECT_ERROR,%s,%s,%u\n", NODE_TTL_GENERATOR, address,
           error);
    (void)k_work_submit(&ttl_generator_scan_restart_work);
    return;
  }

  struct bt_conn_info role_info;
  if ((bt_conn_get_info(conn, &role_info) == 0) &&
      (role_info.role == BT_CONN_ROLE_PERIPHERAL)) {
    k_mutex_lock(&host_connection_mutex, K_FOREVER);
    if (host_connection != NULL) {
      bt_conn_unref(host_connection);
    }
    host_connection = bt_conn_ref(conn);
    k_mutex_unlock(&host_connection_mutex);
    stream_log_link(NODE_HOST, "BLE", "peripheral", "CONNECTED", address, 0U,
                    0U);
    return;
  }

  atomic_clear(&ttl_generator_connecting);
  atomic_clear(&ttl_generator_connection_active);
  k_mutex_lock(&ttl_generator_state_mutex, K_FOREVER);

  if (ttl_generator_client.conn != NULL) {
    bt_conn_unref(ttl_generator_client.conn);
  }

  ttl_generator_client.conn = bt_conn_ref(conn);
  ttl_generator_client.conn_index = bt_conn_index(conn);
  ttl_generator_client.discovery_stage = TTL_GENERATOR_DISCOVERY_NONE;
  ttl_generator_client.service_end_handle = 0U;
  ttl_generator_client.ttl_control_handle = 0U;
  ttl_generator_client.value_handle = 0U;

  (void)atomic_inc(&ttl_generator_connection_generation);
  ttl_generator_anchor_cache_reset();
  sync_node_reset(&ttl_generator_node, "ttl_generator_reconnected");

  struct bt_conn_info info;
  uint32_t interval_us = TTL_GENERATOR_DEFAULT_CONN_INTERVAL_US;

  if (bt_conn_get_info(conn, &info) == 0) {
    interval_us = info.le.interval_us;
  }

  ttl_generator_node.nominal_interval_ticks =
      (uint64_t)interval_us * TTL_GENERATOR_CONTROLLER_TICKS_PER_US;

  k_mutex_unlock(&ttl_generator_state_mutex);
  atomic_set(&ttl_generator_connection_active, 1);

  stream_log_link(NODE_TTL_GENERATOR, "BLE", "central", "CONNECTED", address,
                  interval_us, 0U);

  memset(&ttl_generator_client.exchange, 0, sizeof(ttl_generator_client.exchange));
  ttl_generator_client.exchange.func = ttl_generator_mtu_exchange_callback;

  const int exchange_error =
      bt_gatt_exchange_mtu(conn, &ttl_generator_client.exchange);

  if (exchange_error == -EALREADY) {
    const uint16_t mtu = bt_gatt_get_mtu(conn);

    printk("TTL_GENERATOR_MTU,%s,ALREADY,%u\n", NODE_TTL_GENERATOR, mtu);

    if (mtu < (REMOTE_FRAME_SIZE + 3U)) {
      printk("TTL_GENERATOR_MTU_ERROR,%s,TOO_SMALL,%u\n", NODE_TTL_GENERATOR, mtu);
      (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
      return;
    }

    (void)ttl_generator_discovery_start(conn);
  } else if (exchange_error != 0) {
    printk("TTL_GENERATOR_MTU_ERROR,%s,START,%d\n", NODE_TTL_GENERATOR, exchange_error);
    (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
  }
}

static void bluetooth_disconnected(struct bt_conn *conn, uint8_t reason) {
  char address[BT_ADDR_LE_STR_LEN];
  bt_addr_le_to_str(bt_conn_get_dst(conn), address, sizeof(address));

  bool was_host = false;
  k_mutex_lock(&host_connection_mutex, K_FOREVER);
  if (host_connection == conn) {
    bt_conn_unref(host_connection);
    host_connection = NULL;
    was_host = true;
  }
  k_mutex_unlock(&host_connection_mutex);

  if (was_host) {
    stream_log_link(NODE_HOST, "BLE", "peripheral", "DISCONNECTED", address,
                    0U, reason);

    atomic_clear(&host_notifications_enabled);
    host_command.active = false;

    k_msgq_purge(&host_request_queue);
    k_msgq_purge(&host_notify_queue);
    k_sem_reset(&host_notify_complete_sem);

    (void)k_work_cancel_delayable(&host_command_timeout_work);

    (void)host_advertising_start();
    return;
  }

  stream_log_link(NODE_TTL_GENERATOR, "BLE", "central", "DISCONNECTED", address, 0U,
                  reason);

  atomic_clear(&ttl_generator_connection_active);
  k_mutex_lock(&ttl_generator_state_mutex, K_FOREVER);

  if (ttl_generator_client.conn != NULL) {
    bt_conn_unref(ttl_generator_client.conn);
    ttl_generator_client.conn = NULL;
  }

  memset(&ttl_generator_client.discover, 0, sizeof(ttl_generator_client.discover));
  memset(&ttl_generator_client.subscribe, 0, sizeof(ttl_generator_client.subscribe));
  memset(&ttl_generator_client.exchange, 0, sizeof(ttl_generator_client.exchange));
  ttl_generator_client.discovery_stage = TTL_GENERATOR_DISCOVERY_NONE;
  ttl_generator_client.ttl_control_handle = 0U;
  (void)k_work_cancel_delayable(&ttl_schedule_work);
  (void)k_work_cancel_delayable(&ttl_timeout_work);

  const k_spinlock_key_t ttl_key = k_spin_lock(&ttl_test_lock);
  memset(&ttl_test, 0, sizeof(ttl_test));
  k_spin_unlock(&ttl_test_lock, ttl_key);

  ttl_generator_anchor_cache_reset();
  sync_node_reset(&ttl_generator_node, "ttl_generator_disconnect");
  atomic_clear(&ttl_generator_connecting);

  k_mutex_unlock(&ttl_generator_state_mutex);
}

/*
 * At disconnected() time the Bluetooth host still owns a reference to the
 * connection object.  Start the next scan only once that object is recycled.
 */
static void bluetooth_connection_recycled(void) {
  (void)k_work_submit(&ttl_generator_scan_restart_work);
}

static void bluetooth_parameters_updated(struct bt_conn *conn,
                                         uint16_t interval, uint16_t latency,
                                         uint16_t timeout) {
  ARG_UNUSED(latency);
  ARG_UNUSED(timeout);

  bool is_host = false;
  k_mutex_lock(&host_connection_mutex, K_FOREVER);
  is_host = (host_connection == conn);
  k_mutex_unlock(&host_connection_mutex);

  const uint32_t interval_us = (uint32_t)interval * 1250U;

  if (is_host) {
    stream_log_link(NODE_HOST, "BLE", "peripheral", "PARAMS", "host",
                    interval_us, 0U);
    return;
  }

  const uint64_t new_nominal_interval_ticks =
      (uint64_t)interval_us * TTL_GENERATOR_CONTROLLER_TICKS_PER_US;

  k_mutex_lock(&ttl_generator_state_mutex, K_FOREVER);

  if (ttl_generator_node.nominal_interval_ticks != new_nominal_interval_ticks) {
    ttl_generator_node.nominal_interval_ticks = new_nominal_interval_ticks;
    sync_node_reset(&ttl_generator_node, "ttl_generator_connection_interval_changed");
    ttl_generator_anchor_cache_reset();
  }

  k_mutex_unlock(&ttl_generator_state_mutex);

  stream_log_link(NODE_TTL_GENERATOR, "BLE", "central", "PARAMS",
                  TTL_GENERATOR_ADVERTISED_NAME, interval_us, 0U);
}

static struct bt_conn_cb bluetooth_callbacks = {
    .connected = bluetooth_connected,
    .disconnected = bluetooth_disconnected,
    .recycled = bluetooth_connection_recycled,
    .le_param_updated = bluetooth_parameters_updated,
};

static bool ttl_generator_controller_anchor_event(struct net_buf_simple *buffer) {
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

  if ((atomic_get(&ttl_generator_connection_active) == 0) ||
      (connection_index != ttl_generator_client.conn_index)) {
    return true;
  }

  const uint64_t central_ticks =
      event->anchor_point_us * TTL_GENERATOR_CONTROLLER_TICKS_PER_US;
  const uint32_t generation =
      (uint32_t)atomic_get(&ttl_generator_connection_generation);

  ttl_generator_cache_store_central(event->event_counter, central_ticks,
                                generation);
  return true;
}

static int hub_bluetooth_init(void) {
  int error = bt_enable(NULL);
  if (error != 0) {
    return error;
  }

  error = ttl_generator_controller_clock_init();
  if (error != 0) {
    return error;
  }

  bt_conn_cb_register(&bluetooth_callbacks);

  error = bt_hci_register_vnd_evt_cb(ttl_generator_controller_anchor_event);
  if (error != 0) {
    return error;
  }

  sdc_hci_cmd_vs_conn_anchor_point_update_event_report_enable_t enable = {
      .enable = true,
  };

  error = hci_vs_sdc_conn_anchor_point_update_event_report_enable(&enable);
  if (error != 0) {
    return error;
  }

  error = host_advertising_start();
  if (error != 0) {
    return error;
  }

  return ttl_generator_scan_start();
}

/* -------------------------------------------------------------------------- */
/* Hardware clock, pulse generation and node work                      */
/* -------------------------------------------------------------------------- */

static void hub_timer_event_handler(nrf_timer_event_t event_type,
                                       void *context) {
  ARG_UNUSED(context);

  if (event_type == NRF_TIMER_EVENT_COMPARE0) {
    (void)atomic_inc(&hub_sync_pulse_count);
    (void)k_work_submit(&node_window_open_work);
  }
}

static void hub_timer_irq_wrapper(const void *argument) {
  nrfx_timer_irq_handler((const nrfx_timer_t *)argument);
}

static uint64_t hub_event_ticks_from_phase(uint32_t captured_phase) {
  const unsigned int key = irq_lock();

  uint32_t pulse_count = (uint32_t)atomic_get(&hub_sync_pulse_count);
  const uint32_t current_phase = hub_timer_phase_ticks();
  const bool boundary_pending =
      nrf_timer_event_check(hub_timer.p_reg, NRF_TIMER_EVENT_COMPARE0);

  /* COMPARE0 cleared TIMER2 before its ISR incremented the pulse count. */
  if (boundary_pending &&
      (current_phase < NODE_SYNC_ACCEPTANCE_WINDOW_TICKS)) {
    pulse_count++;
  }

  /* Capture belongs to the period immediately before the current one. */
  if ((captured_phase > current_phase) &&
      ((captured_phase - current_phase) > (HUB_SYNC_PERIOD_TICKS / 2U)) &&
      (pulse_count > 0U)) {
    pulse_count--;
  }

  irq_unlock(key);

  return ((uint64_t)pulse_count * (uint64_t)HUB_SYNC_PERIOD_TICKS) +
         (uint64_t)captured_phase;
}

static void hub_event_gpio_handler(nrfx_gpiote_pin_t pin,
                                      nrfx_gpiote_trigger_t trigger,
                                      void *context) {
  ARG_UNUSED(pin);
  ARG_UNUSED(trigger);
  ARG_UNUSED(context);

  const uint32_t captured_phase =
      nrf_timer_cc_get(hub_timer.p_reg, NRF_TIMER_CC_CHANNEL3);

  const uint32_t event_id = (uint32_t)atomic_inc(&hub_event_sequence) + 1U;

  const uint64_t hub_ticks = hub_event_ticks_from_phase(captured_phase);

  hub_event_reference_store(event_id, hub_ticks);

  const struct hub_event_record record = {
      .event_id = event_id,
      .hub_ticks = hub_ticks,
  };

  if (k_msgq_put(&hub_event_queue, &record, K_NO_WAIT) != 0) {
    (void)atomic_inc(&hub_event_drop_count);
    return;
  }

  (void)k_work_submit(&hub_event_work);
}

static void hub_event_work_handler(struct k_work *work) {
  ARG_UNUSED(work);

  struct hub_event_record record;

  while (k_msgq_get(&hub_event_queue, &record, K_NO_WAIT) == 0) {
    stream_log_event(NODE_HUB, record.event_id, EVENT_KIND_GPIO_RISE,
                     record.hub_ticks, HUB_TIMER_HZ,
                     (int64_t)record.hub_ticks, EVENT_STATE_LOCAL, 0ULL);
  }
}

static int hub_gpiote_init(void) {
  IRQ_CONNECT(DT_IRQN(GPIOTE_NODE), DT_IRQ(GPIOTE_NODE, priority),
              nrfx_gpiote_irq_handler, hub_gpiote, 0);

  if (nrfx_gpiote_init_check(hub_gpiote)) {
    return 0;
  }

  return nrfx_gpiote_init(hub_gpiote, 0);
}

static int hub_event_input_init(void) {
  int error =
      nrfx_gpiote_channel_alloc(hub_gpiote, &hub_event_input_channel);

  if (error != 0) {
    return error;
  }

  static const nrf_gpio_pin_pull_t pull = NRF_GPIO_PIN_PULLDOWN;

  const nrfx_gpiote_trigger_config_t trigger_config = {
      .trigger = NRFX_GPIOTE_TRIGGER_LOTOHI,
      .p_in_channel = &hub_event_input_channel,
  };

  const nrfx_gpiote_handler_config_t handler_config = {
      .handler = hub_event_gpio_handler,
      .p_context = NULL,
  };

  const nrfx_gpiote_input_pin_config_t input_config = {
      .p_pull_config = &pull,
      .p_trigger_config = &trigger_config,
      .p_handler_config = &handler_config,
  };

  error = nrfx_gpiote_input_configure(hub_gpiote, EVENT_INPUT_PIN,
                                      &input_config);

  if (error != 0) {
    return error;
  }

  error = nrfx_gppi_conn_alloc(
      nrfx_gpiote_in_event_address_get(hub_gpiote, EVENT_INPUT_PIN),
      nrf_timer_task_address_get(hub_timer.p_reg, NRF_TIMER_TASK_CAPTURE3),
      &hub_event_capture_connection);

  if (error != 0) {
    return error;
  }

  nrfx_gppi_conn_enable(hub_event_capture_connection);
  nrfx_gpiote_trigger_enable(hub_gpiote, EVENT_INPUT_PIN, true);

  return 0;
}

static void ttl_capture_timer_event_handler(nrf_timer_event_t event_type,
                                            void *context) {
  ARG_UNUSED(event_type);
  ARG_UNUSED(context);
}

static void ttl_capture_timer_irq_wrapper(const void *argument) {
  nrfx_timer_irq_handler((const nrfx_timer_t *)argument);
}

static void ttl_input_gpio_handler(nrfx_gpiote_pin_t pin,
                                   nrfx_gpiote_trigger_t trigger,
                                   void *context) {
  ARG_UNUSED(pin);
  ARG_UNUSED(trigger);
  ARG_UNUSED(context);

  const uint32_t captured_phase =
      nrf_timer_cc_get(ttl_capture_timer.p_reg, NRF_TIMER_CC_CHANNEL0);
  const uint64_t acquired_hub_ticks =
      hub_event_ticks_from_phase(captured_phase);

  struct ttl_capture_record record = {0};
  bool accepted = false;

  const k_spinlock_key_t key = k_spin_lock(&ttl_test_lock);

  if (ttl_test.pending && !ttl_test.acquired_seen) {
    ttl_test.acquired_seen = true;
    ttl_test.acquired_hub_ticks = acquired_hub_ticks;
    record.sequence = ttl_test.sequence;
    record.target_hub_ticks = ttl_test.target_hub_ticks;
    record.acquired_hub_ticks = acquired_hub_ticks;
    accepted = true;
  }

  k_spin_unlock(&ttl_test_lock, key);

  if (!accepted) {
    return;
  }

  if (k_msgq_put(&ttl_capture_queue, &record, K_NO_WAIT) != 0) {
    return;
  }

  (void)k_work_submit(&ttl_capture_work);
}

static void ttl_capture_work_handler(struct k_work *work) {
  ARG_UNUSED(work);

  struct ttl_capture_record record;

  while (k_msgq_get(&ttl_capture_queue, &record, K_NO_WAIT) == 0) {
    (void)k_work_submit(&ttl_result_work);
  }
}

static void ttl_result_work_handler(struct k_work *work) {
  ARG_UNUSED(work);

  bool log_generated = false;
  bool log_acquired = false;
  bool log_result = false;
  struct ttl_test_state snapshot = {0};

  const k_spinlock_key_t key = k_spin_lock(&ttl_test_lock);

  if (!ttl_test.pending) {
    k_spin_unlock(&ttl_test_lock, key);
    return;
  }

  if (ttl_test.generated_seen && !ttl_test.generated_logged) {
    ttl_test.generated_logged = true;
    log_generated = true;
  }

  /*
   * The physical input normally arrives before the generated record returns
   * over Bluetooth. Buffer it so the serial stream presents the logical order:
   * generated first, acquired second, then the combined result.
   */
  if (ttl_test.generated_seen && ttl_test.acquired_seen &&
      !ttl_test.acquired_logged) {
    ttl_test.acquired_logged = true;
    log_acquired = true;
  }

  if (ttl_test.generated_seen && ttl_test.acquired_seen &&
      !ttl_test.result_logged) {
    ttl_test.result_logged = true;
    log_result = true;
  }

  snapshot = ttl_test;

  if (log_result) {
    ttl_test.pending = false;
  }

  k_spin_unlock(&ttl_test_lock, key);

  const int64_t total_error_ticks =
      (int64_t)snapshot.acquired_hub_ticks - (int64_t)snapshot.target_hub_ticks;

  if (log_generated) {
    const int64_t generation_error_ticks =
        (snapshot.generated_hub_ticks >= 0)
            ? snapshot.generated_hub_ticks - (int64_t)snapshot.target_hub_ticks
            : 0;

    printk("TTL_PULSE_GENERATED,%s,%u,%llu,%lld,%llu,%lld\n", NODE_TTL_GENERATOR,
           snapshot.sequence,
           (unsigned long long)snapshot.generated_local_ticks,
           (long long)snapshot.generated_hub_ticks,
           (unsigned long long)snapshot.target_hub_ticks,
           (long long)((snapshot.generated_hub_ticks >= 0)
                           ? ticks_to_ns((double)generation_error_ticks)
                           : INT64_MIN));
  }

  if (log_acquired) {
    printk("TTL_PULSE_ACQUIRED,%s,%u,%llu,%llu,%lld\n", NODE_HUB,
           snapshot.sequence, (unsigned long long)snapshot.acquired_hub_ticks,
           (unsigned long long)snapshot.target_hub_ticks,
           (long long)ticks_to_ns((double)total_error_ticks));
  }

  if (log_result) {
    const int64_t generation_error_ticks =
        (snapshot.generated_hub_ticks >= 0)
            ? snapshot.generated_hub_ticks - (int64_t)snapshot.target_hub_ticks
            : 0;
    const int64_t wire_offset_ticks =
        (snapshot.generated_hub_ticks >= 0)
            ? (int64_t)snapshot.acquired_hub_ticks -
                  snapshot.generated_hub_ticks
            : 0;

    printk("TTL_PULSE_RESULT,%u,%llu,%llu,%lld,%llu,%lld,%lld,%lld\n",
           snapshot.sequence, (unsigned long long)snapshot.target_hub_ticks,
           (unsigned long long)snapshot.target_local_ticks,
           (long long)snapshot.generated_hub_ticks,
           (unsigned long long)snapshot.acquired_hub_ticks,
           (long long)((snapshot.generated_hub_ticks >= 0)
                           ? ticks_to_ns((double)generation_error_ticks)
                           : INT64_MIN),
           (long long)((snapshot.generated_hub_ticks >= 0)
                           ? ticks_to_ns((double)wire_offset_ticks)
                           : INT64_MIN),
           (long long)ticks_to_ns((double)total_error_ticks));

    (void)k_work_cancel_delayable(&ttl_timeout_work);
    (void)k_work_reschedule(&ttl_schedule_work, K_MSEC(TTL_TEST_PERIOD_MS));
  }
}

static void ttl_schedule_work_handler(struct k_work *work) {
  ARG_UNUSED(work);

  if ((atomic_get(&ttl_generator_connection_active) == 0) ||
      (ttl_generator_client.ttl_control_handle == 0U)) {
    (void)k_work_reschedule(&ttl_schedule_work, K_MSEC(TTL_TEST_RETRY_MS));
    return;
  }

  const k_spinlock_key_t state_key = k_spin_lock(&ttl_test_lock);
  const bool busy = ttl_test.pending;
  k_spin_unlock(&ttl_test_lock, state_key);

  if (busy) {
    return;
  }

  uint64_t target_local_ticks;
  const uint64_t requested_hub_ticks =
      hub_time_now_ticks() +
      ((uint64_t)TTL_TEST_LEAD_MS * HUB_TIMER_HZ / 1000ULL);

  if (!ttl_generator_model_inverse(requested_hub_ticks, &target_local_ticks)) {
    (void)k_work_reschedule(&ttl_schedule_work, K_MSEC(TTL_TEST_RETRY_MS));
    return;
  }

  bool model_valid = false;
  const int64_t quantized_target_hub =
      ttl_generator_local_to_hub(target_local_ticks, &model_valid);

  if (!model_valid || (quantized_target_hub < 0)) {
    (void)k_work_reschedule(&ttl_schedule_work, K_MSEC(TTL_TEST_RETRY_MS));
    return;
  }

  const uint32_t sequence = (uint32_t)atomic_inc(&ttl_test_sequence) + 1U;

  uint8_t command[TTL_COMMAND_SIZE] = {0};
  sys_put_le16(TTL_COMMAND_MAGIC, &command[TTL_OFFSET_MAGIC]);
  command[TTL_OFFSET_VERSION] = TTL_COMMAND_VERSION;
  command[TTL_OFFSET_OPCODE] = TTL_COMMAND_OPCODE_SCHEDULE;
  sys_put_le32(sequence, &command[TTL_OFFSET_SEQUENCE]);
  sys_put_le64(target_local_ticks, &command[TTL_OFFSET_TARGET_TICKS]);
  sys_put_le32(TTL_TEST_PULSE_WIDTH_US, &command[TTL_OFFSET_PULSE_WIDTH_US]);

  struct bt_conn *conn = NULL;

  k_mutex_lock(&ttl_generator_state_mutex, K_FOREVER);
  if (ttl_generator_client.conn != NULL) {
    conn = bt_conn_ref(ttl_generator_client.conn);
  }
  k_mutex_unlock(&ttl_generator_state_mutex);

  if (conn == NULL) {
    (void)k_work_reschedule(&ttl_schedule_work, K_MSEC(TTL_TEST_RETRY_MS));
    return;
  }

  const k_spinlock_key_t key = k_spin_lock(&ttl_test_lock);
  memset(&ttl_test, 0, sizeof(ttl_test));
  ttl_test.pending = true;
  ttl_test.sequence = sequence;
  ttl_test.target_hub_ticks = (uint64_t)quantized_target_hub;
  ttl_test.target_local_ticks = target_local_ticks;
  ttl_test.generated_hub_ticks = -1;
  k_spin_unlock(&ttl_test_lock, key);

  const int error =
      bt_gatt_write_without_response(conn, ttl_generator_client.ttl_control_handle,
                                     command, sizeof(command), false);
  bt_conn_unref(conn);

  if (error != 0) {
    const k_spinlock_key_t clear_key = k_spin_lock(&ttl_test_lock);
    memset(&ttl_test, 0, sizeof(ttl_test));
    k_spin_unlock(&ttl_test_lock, clear_key);

    stream_log_fault(NODE_TTL_GENERATOR, "TTL_WRITE", error, sequence);
    (void)k_work_reschedule(&ttl_schedule_work, K_MSEC(TTL_TEST_RETRY_MS));
    return;
  }

  printk("TTL_PULSE_SCHEDULED,%s,%u,%llu,%llu,%u\n", NODE_HUB, sequence,
         (unsigned long long)quantized_target_hub,
         (unsigned long long)target_local_ticks, TTL_TEST_PULSE_WIDTH_US);

  (void)k_work_reschedule(
      &ttl_timeout_work,
      K_MSEC(TTL_TEST_LEAD_MS + TTL_TEST_TIMEOUT_AFTER_TARGET_MS));
}

static void ttl_timeout_work_handler(struct k_work *work) {
  ARG_UNUSED(work);

  struct ttl_test_state snapshot = {0};
  bool timed_out = false;

  const k_spinlock_key_t key = k_spin_lock(&ttl_test_lock);

  if (ttl_test.pending) {
    snapshot = ttl_test;
    memset(&ttl_test, 0, sizeof(ttl_test));
    timed_out = true;
  }

  k_spin_unlock(&ttl_test_lock, key);

  if (timed_out) {
    printk("TTL_PULSE_TIMEOUT,%u,%llu,%llu,%u,%u\n", snapshot.sequence,
           (unsigned long long)snapshot.target_hub_ticks,
           (unsigned long long)hub_time_now_ticks(),
           snapshot.generated_seen ? 1U : 0U, snapshot.acquired_seen ? 1U : 0U);
  }

  (void)k_work_reschedule(&ttl_schedule_work, K_MSEC(TTL_TEST_PERIOD_MS));
}

static int hub_ttl_input_init(void) {
  IRQ_CONNECT(DT_IRQN(TTL_CAPTURE_TIMER_NODE),
              DT_IRQ(TTL_CAPTURE_TIMER_NODE, priority),
              ttl_capture_timer_irq_wrapper, &ttl_capture_timer, 0);

  nrfx_timer_config_t timer_config = NRFX_TIMER_DEFAULT_CONFIG(HUB_TIMER_HZ);
  timer_config.bit_width = NRF_TIMER_BIT_WIDTH_32;
  timer_config.interrupt_priority = DT_IRQ(TTL_CAPTURE_TIMER_NODE, priority);

  int error = nrfx_timer_init(&ttl_capture_timer, &timer_config,
                              ttl_capture_timer_event_handler);
  if (error != 0) {
    return error;
  }

  nrfx_timer_clear(&ttl_capture_timer);
  nrfx_timer_enable(&ttl_capture_timer);

  /* TIMER3 is phase aligned to TIMER2 at every 4 Hz TIMER2 boundary. */
  error = nrfx_gppi_conn_alloc(
      nrfx_timer_compare_event_address_get(&hub_timer,
                                           NRF_TIMER_CC_CHANNEL0),
      nrf_timer_task_address_get(ttl_capture_timer.p_reg, NRF_TIMER_TASK_CLEAR),
      &ttl_timer_clear_connection);
  if (error != 0) {
    return error;
  }
  nrfx_gppi_conn_enable(ttl_timer_clear_connection);

  error = nrfx_gpiote_channel_alloc(hub_gpiote, &ttl_input_gpiote_channel);
  if (error != 0) {
    return error;
  }

  static const nrf_gpio_pin_pull_t pull = NRF_GPIO_PIN_PULLDOWN;
  const nrfx_gpiote_trigger_config_t trigger_config = {
      .trigger = NRFX_GPIOTE_TRIGGER_LOTOHI,
      .p_in_channel = &ttl_input_gpiote_channel,
  };
  const nrfx_gpiote_handler_config_t handler_config = {
      .handler = ttl_input_gpio_handler,
      .p_context = NULL,
  };
  const nrfx_gpiote_input_pin_config_t input_config = {
      .p_pull_config = &pull,
      .p_trigger_config = &trigger_config,
      .p_handler_config = &handler_config,
  };

  error =
      nrfx_gpiote_input_configure(hub_gpiote, TTL_INPUT_PIN, &input_config);
  if (error != 0) {
    return error;
  }

  error = nrfx_gppi_conn_alloc(
      nrfx_gpiote_in_event_address_get(hub_gpiote, TTL_INPUT_PIN),
      nrf_timer_task_address_get(ttl_capture_timer.p_reg,
                                 NRF_TIMER_TASK_CAPTURE0),
      &ttl_input_capture_connection);
  if (error != 0) {
    return error;
  }

  nrfx_gppi_conn_enable(ttl_input_capture_connection);
  nrfx_gpiote_trigger_enable(hub_gpiote, TTL_INPUT_PIN, true);

  printk("TTL_INPUT_READY,%s,pin=%u,timer=3,cc=0\n", NODE_HUB,
         (unsigned int)TTL_INPUT_PIN);
  return 0;
}

static int hub_hfxo_acquire(void) {
  if (hub_hfclk_reserved) {
    return 0;
  }

  struct onoff_manager *const manager =
      z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);

  if (manager == NULL) {
    return -ENODEV;
  }

  sys_notify_init_spinwait(&hub_hfclk_client.notify);

  int error = onoff_request(manager, &hub_hfclk_client);
  if (error < 0) {
    return error;
  }

  int result = -EINPROGRESS;

  do {
    error = sys_notify_fetch_result(&hub_hfclk_client.notify, &result);
  } while (error == -EAGAIN);

  if (error != 0) {
    return error;
  }

  if (result != 0) {
    return result;
  }

  nrf_clock_hfclk_t source = NRF_CLOCK_HFCLK_LOW_ACCURACY;
  const bool running =
      nrf_clock_is_running(NRF_CLOCK, NRF_CLOCK_DOMAIN_HFCLK, &source);

  if (!running || (source != NRF_CLOCK_HFCLK_HIGH_ACCURACY)) {
    (void)onoff_release(manager);
    return -EIO;
  }

  /* Deliberately retain the manager reference for this boot session. */
  hub_hfclk_reserved = true;
  printk("HFCLK,%s,1,XTAL\n", NODE_HUB);
  return 0;
}

static int hub_sync_output_init(void) {
#if !defined(GPIOTE_FEATURE_SET_PRESENT) || !defined(GPIOTE_FEATURE_CLR_PRESENT)
#error "This hub implementation requires GPIOTE SET and CLR tasks"
#endif

  int error;

  error = nrfx_gpiote_channel_alloc(hub_gpiote, &hub_sync_output_channel);
  if (error != 0) {
    return error;
  }

  static const nrfx_gpiote_output_config_t output_config = {
      .drive = NRF_GPIO_PIN_S0S1,
      .input_connect = NRF_GPIO_PIN_INPUT_DISCONNECT,
      .pull = NRF_GPIO_PIN_NOPULL,
  };

  const nrfx_gpiote_task_config_t task_config = {
      .task_ch = hub_sync_output_channel,
      .polarity = NRF_GPIOTE_POLARITY_TOGGLE,
      .init_val = NRF_GPIOTE_INITIAL_VALUE_HIGH,
  };

  error = nrfx_gpiote_output_configure(hub_gpiote, SYNC_OUTPUT_PIN,
                                       &output_config, &task_config);
  if (error != 0) {
    return error;
  }

  nrfx_gpiote_out_task_enable(hub_gpiote, SYNC_OUTPUT_PIN);
  return 0;
}

static int hub_timer_init(void) {
  IRQ_CONNECT(DT_IRQN(TIMER_NODE), DT_IRQ(TIMER_NODE, priority),
              hub_timer_irq_wrapper, &hub_timer, 0);

  nrfx_timer_config_t timer_config = NRFX_TIMER_DEFAULT_CONFIG(HUB_TIMER_HZ);
  timer_config.bit_width = NRF_TIMER_BIT_WIDTH_32;
  timer_config.interrupt_priority = DT_IRQ(TIMER_NODE, priority);

  int error =
      nrfx_timer_init(&hub_timer, &timer_config, hub_timer_event_handler);
  if (error != 0) {
    return error;
  }

  nrfx_timer_extended_compare(&hub_timer, NRF_TIMER_CC_CHANNEL0,
                              HUB_SYNC_PERIOD_TICKS,
                              NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK, true);

  nrfx_timer_compare(&hub_timer, NRF_TIMER_CC_CHANNEL1,
                     HUB_SYNC_PULSE_WIDTH_TICKS, false);

  /*
   * COMPARE0 marks the synchronization timestamp.
   * Pull the Grove driver input low to begin the active-low pulse.
   */
  error = nrfx_gppi_conn_alloc(
      nrfx_timer_compare_event_address_get(&hub_timer,
                                           NRF_TIMER_CC_CHANNEL0),
      nrfx_gpiote_clr_task_address_get(hub_gpiote, SYNC_OUTPUT_PIN),
      &hub_sync_assert_connection);
  if (error != 0) {
    return error;
  }

  /*
   * Return the Grove driver input high after the configured pulse width.
   */
  error = nrfx_gppi_conn_alloc(
      nrfx_timer_compare_event_address_get(&hub_timer,
                                           NRF_TIMER_CC_CHANNEL1),
      nrfx_gpiote_set_task_address_get(hub_gpiote, SYNC_OUTPUT_PIN),
      &hub_sync_release_connection);
  if (error != 0) {
    return error;
  }

  nrfx_gppi_conn_enable(hub_sync_assert_connection);
  nrfx_gppi_conn_enable(hub_sync_release_connection);

  nrfx_timer_clear(&hub_timer);
  nrfx_timer_enable(&hub_timer);
  return 0;
}

static void node_drain_startup_queue(void) {
  unsigned int read_errors = 0U;

  for (unsigned int attempt = 0U; attempt < 64U; ++attempt) {
    struct remote_frame snapshot;
    const int error = node_read_frame(&snapshot);

    if (error != 0) {
      read_errors++;
      k_sleep(K_MSEC(20));
      continue;
    }

    node_handle_status(&snapshot);

    if ((snapshot.status_flags & REMOTE_STATUS_RECORD_VALID) == 0U) {
      printk("PORT_READY,%s,0x%02x,%u,%u\n", NODE_NODE, snapshot.status_flags,
             snapshot.capture_loss_count, snapshot.transport_error_count);
      return;
    }

    printk("STARTUP_RECORD_DROP,%s,%u,%llu,%u\n", NODE_NODE,
           snapshot.record_type, (unsigned long long)snapshot.capture_ticks,
           snapshot.pending_count);
  }

  printk("PORT_STARTUP_DRAIN_INCOMPLETE,%s,%u\n", NODE_NODE, read_errors);
  sync_node_reset(&node_node, "startup_drain_incomplete");
}

static void node_window_open_work_handler(struct k_work *work) {
  ARG_UNUSED(work);

  const uint32_t pulse_number = (uint32_t)atomic_get(&hub_sync_pulse_count);

  if (pulse_number == node_last_opened_pulse) {
    return;
  }

  if ((node_last_opened_pulse != 0U) &&
      (pulse_number != (node_last_opened_pulse + 1U))) {
    const uint64_t value =
        ((uint64_t)node_last_opened_pulse << 32) | pulse_number;

    stream_log_fault(NODE_NODE, "WINDOW_WORK_OVERRUN", -EOVERFLOW, value);
    sync_node_reset(&node_node, "hub_window_work_overrun");
    node_fault_history_reset();
  }

  if (node_sync_window_state.open) {
    node_sync_window_reject(node_sync_window_state.pulse_number,
                             "previous_window_not_closed");
  }

  node_last_opened_pulse = pulse_number;
  memset(&node_sync_window_state, 0, sizeof(node_sync_window_state));
  node_sync_window_state.open = true;
  node_sync_window_state.pulse_number = pulse_number;
  node_sync_window_state.hub_ticks =
      (uint64_t)pulse_number * (uint64_t)HUB_SYNC_PERIOD_TICKS;

  const uint32_t phase = hub_timer_phase_ticks();

  if (phase >= NODE_SYNC_ACCEPTANCE_WINDOW_TICKS) {
    node_sync_window_state.open = false;
    node_sync_window_reject(pulse_number, "hub_opened_window_late");
    (void)k_work_reschedule(&node_poll_work, K_NO_WAIT);
    return;
  }

  hub_schedule_at_phase(&node_poll_work, NODE_FIRST_POLL_TICKS);
  hub_schedule_at_phase(&node_window_close_work,
                           NODE_SYNC_ACCEPTANCE_WINDOW_TICKS);
}

static void node_poll_work_handler(struct k_work *work) {
  ARG_UNUSED(work);

  const uint32_t phase_before_read = hub_timer_phase_ticks();
  if (phase_before_read >= NODE_FINAL_DRAIN_PHASE_TICKS) {
    return;
  }

  const uint32_t pulse_before_read =
      (uint32_t)atomic_get(&hub_sync_pulse_count);

  struct remote_frame snapshot;
  const int error = node_read_frame(&snapshot);

  const uint32_t pulse_after_read =
      (uint32_t)atomic_get(&hub_sync_pulse_count);
  const uint32_t phase_after_read = hub_timer_phase_ticks();

  if (error != 0) {
    node_consecutive_transport_errors++;
    printk("RS485_READ_ERROR,%s,%u,%d,%u\n", NODE_NODE, pulse_after_read,
           error, node_consecutive_transport_errors);

    if (node_sync_window_state.open &&
        (node_consecutive_transport_errors >=
         NODE_MAX_CONSECUTIVE_TRANSPORT_ERRORS)) {
      node_sync_window_state.invalid = true;
      sync_node_reset(&node_node, "repeated_rs485_read_error");
    }

    node_schedule_next_poll(phase_after_read, 0U);
    return;
  }

  node_consecutive_transport_errors = 0U;
  node_handle_status(&snapshot);
  node_process_frame(&snapshot, pulse_before_read, pulse_after_read,
                      phase_after_read);
  node_schedule_next_poll(phase_after_read, snapshot.pending_count);
}

static void node_window_close_work_handler(struct k_work *work) {
  ARG_UNUSED(work);

  if (!node_sync_window_state.open) {
    return;
  }

  if ((uint32_t)atomic_get(&hub_sync_pulse_count) !=
      node_sync_window_state.pulse_number) {
    return;
  }

  const uint32_t phase = hub_timer_phase_ticks();

  if (phase < NODE_SYNC_ACCEPTANCE_WINDOW_TICKS) {
    hub_schedule_at_phase(&node_window_close_work,
                             NODE_SYNC_ACCEPTANCE_WINDOW_TICKS);
    return;
  }

  /* One bridge observation per 4 Hz hardware-sync interval. */
  ttl_generator_controller_bridge_sample();

  const uint32_t pulse_number = node_sync_window_state.pulse_number;
  const uint64_t hub_ticks = node_sync_window_state.hub_ticks;
  const bool invalid = node_sync_window_state.invalid;
  const bool candidate_valid = node_sync_window_state.candidate_valid;
  const struct remote_frame candidate = node_sync_window_state.candidate;

  node_sync_window_state.open = false;

  if (invalid) {
    node_sync_window_reject(pulse_number, "window_invalid");
  } else if (!candidate_valid) {
    node_sync_window_timeout(pulse_number);
  } else {
    node_timeout_history_record(false);
    node_consecutive_bad_windows = 0U;
    (void)sync_node_process_pair(&node_node, pulse_number, hub_ticks,
                                 &candidate);
  }
}

int main(void) {
  printk("SCHEMA,%u\n", STREAM_SCHEMA_VERSION);
  printk("# "
         "PAIR_RAW,node,sync_id,hub_ticks,local_ticks,prospective_count,has_"
         "previous,local_delta_ticks,local_interval_error_ticks,transport_age_"
         "ticks\n");
  printk("# "
         "SYNC,node,sync_id,hub_ticks,local_ticks,status_flags,record_flags,"
         "pending_count,state,slope_ppb,local_reference_ticks,hub_reference_"
         "ticks,rms_ns,prefit_residual_ns,model_step_ns,transport_age_ticks\n");
  printk("# "
         "EVENT,node,event_id,kind,local_ticks,local_hz,hub_ticks,state,"
         "transport_age_ticks\n");
  printk("# "
         "EVENT_MATCH,node,event_id,reference_node,reference_event_id,"
         "converted_hub_ticks,reference_hub_ticks,error_ns\n");
  printk("# LINK,node,transport,role,state,peer,interval_us,reason\n");
  printk("# FAULT,node,category,code,value\n");
  printk("# "
         "TTL_PULSE_SCHEDULED,node,sequence,target_hub_ticks,target_ttl_generator_"
         "ticks,pulse_width_us\n");
  printk("# "
         "TTL_PULSE_GENERATED,node,sequence,generated_local_ticks,generated_"
         "hub_ticks,target_hub_ticks,generation_error_ns\n");
  printk("# "
         "TTL_PULSE_ACQUIRED,node,sequence,acquired_hub_ticks,target_hub_ticks,"
         "total_error_ns\n");
  printk("# "
         "TTL_PULSE_RESULT,sequence,target_hub_ticks,target_ttl_generator_ticks,"
         "generated_hub_ticks,acquired_hub_ticks,generation_error_ns,wire_"
         "offset_ns,total_error_ns\n");

  int error = node_rs485_init();
  if (error != 0) {
    stream_log_fault(NODE_NODE, "RS485_INIT", error, 0ULL);
    return 0;
  }

  printk("NODE_RS485_READY,%s,address=%u,baud=%u,uart=1,de_pin=%u\n",
         NODE_NODE, NODE_RS485_ADDRESS, NODE_RS485_BAUD,
         (unsigned int)NODE_RS485_DE_PIN);

  error = hub_hfxo_acquire();
  if (error != 0) {
    stream_log_fault(NODE_HUB, "HFXO_ACQUIRE", error, 0ULL);
    return 0;
  }

  node_drain_startup_queue();

  error = hub_gpiote_init();
  if (error != 0) {
    stream_log_fault(NODE_HUB, "GPIOTE_INIT", error, 0ULL);
    return 0;
  }

  error = hub_sync_output_init();
  if (error != 0) {
    stream_log_fault(NODE_HUB, "SYNC_OUTPUT_INIT", error, SYNC_OUTPUT_PIN);
    return 0;
  }

  /*
   * Bluetooth must initialize before the RTC2 mirror, because RTC0 is the
   * controller clock being mirrored.  The hardware SYNC timer starts only
   * after this function returns, so bridge sampling cannot race its setup.
   */
  error = hub_bluetooth_init();
  if (error != 0) {
    stream_log_fault(NODE_HUB, "BLUETOOTH_INIT", error, 0ULL);
    return 0;
  }

  error = hub_timer_init();
  if (error != 0) {
    stream_log_fault(NODE_HUB, "TIMER_INIT", error, 0ULL);
    return 0;
  }

  error = hub_event_input_init();
  if (error != 0) {
    stream_log_fault(NODE_HUB, "EVENT_INPUT_INIT", error, EVENT_INPUT_PIN);
    return 0;
  }

  error = hub_ttl_input_init();
  if (error != 0) {
    stream_log_fault(NODE_HUB, "TTL_INPUT_INIT", error, TTL_INPUT_PIN);
    return 0;
  }

  (void)k_work_reschedule(&node_poll_work, K_NO_WAIT);

  printk("READY,%s,%u,%u,rs485:%u@%u,%u,%lld,%s,%u\n", NODE_HUB,
         HUB_TIMER_HZ, HUB_SYNC_PERIOD_TICKS, NODE_RS485_ADDRESS,
         NODE_RS485_BAUD, CLOCK_MODEL_WINDOW_SIZE,
         (long long)math_round_i64(CLOCK_MODEL_MAX_SKEW_PPM),
         TTL_GENERATOR_ADVERTISED_NAME, EVENT_INPUT_PIN);
  printk(
      "TTL_TEST_CONFIG,lead_ms=%u,period_ms=%u,width_us=%u,ttl_input_pin=%u\n",
      TTL_TEST_LEAD_MS, TTL_TEST_PERIOD_MS, TTL_TEST_PULSE_WIDTH_US,
      (unsigned int)TTL_INPUT_PIN);

  while (true) {
    k_sleep(K_SECONDS(60));
  }

  return 0;
}
