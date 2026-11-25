#include "rs485_bus.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <gpiote_nrfx.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_uarte.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "debug_log.hpp"
#include "rig_protocol/bytes.hpp"
#include "rig_protocol/system_config.hpp"
#include "timebase.hpp"

namespace hub_rs485 {
namespace {

#define USER_NODE DT_PATH(zephyr_user)
#define UART_NODE DT_NODELABEL(uart1)
#define RS485_DIR_PIN NRF_DT_GPIOS_TO_PSEL(USER_NODE, rs485_dir_gpios)

const device *uart = DEVICE_DT_GET(UART_NODE);
K_MUTEX_DEFINE(bus_mutex);
atomic_t next_transfer = ATOMIC_INIT(1);
atomic_t error_count;
atomic_t retry_count;
atomic_t timeout_count;
atomic_t decode_error_count;
atomic_t reassembly_error_count;
atomic_t transmit_error_count;
atomic_t receive_stop_count;
atomic_t receive_restart_failure_count;
atomic_t receiver_reset_count;
atomic_t receiver_recovery_count;
atomic_t receiver_kick_count;
atomic_t receiver_wedge_count;

/*
 * Receiver watchdog.
 *
 * A UART error stops reception and the driver callback restarts it. If that
 * restart chain ever breaks, or the UART is left in a state where it sees
 * bytes but never a valid frame, every node drops off the bus and nothing
 * restarts it. Nodes keep resending their unacknowledged records, so the bus
 * never recovers on its own.
 *
 * The watchdog runs in exchange(), under the bus mutex. When the bus had
 * valid traffic recently but none for stuck_after_ms, it stops reception,
 * waits for the driver to confirm, and starts it again. It backs off between
 * attempts so an intentionally empty bus is not reset forever. It never
 * reboots the hub.
 *
 * The known failure is caught much sooner by the wedge check: a UART error
 * (RX_STOPPED) is normally followed by RX_DISABLED within about 0.1 ms. If
 * RX_DISABLED is still missing wedge_after_ms later, the driver has lost the
 * stop and the receiver is reset straight away, before the next attempt.
 * The silence watchdog stays as a backstop for anything else.
 */
inline constexpr std::int64_t stuck_after_ms = 1000;
inline constexpr std::int64_t watchdog_active_for_ms = 60'000;
inline constexpr std::int64_t first_reset_interval_ms = 1000;
inline constexpr std::int64_t max_reset_interval_ms = 32'000;
// A healthy stop completes in a few byte times (~0.1 ms at 460800 baud).
inline constexpr std::int32_t rx_disable_wait_ms = 2;
inline constexpr std::uint32_t wedge_after_ms = 5;

atomic_t receiver_running;
// Set by RX_STOPPED, cleared by RX_DISABLED. stop_pending_since_ms is the
// 32-bit uptime of the RX_STOPPED that is still waiting.
atomic_t stop_pending;
atomic_t stop_pending_since_ms;
// Set while the watchdog stops reception itself, so the RX_DISABLED callback
// leaves the restart to the watchdog.
atomic_t deliberate_stop;
K_SEM_DEFINE(receiver_disabled, 0, 1);
// Bytes and UART stops since the last valid frame, for the stuck report.
atomic_t bytes_since_valid;
atomic_t stops_since_valid;

// Only touched by the thread holding bus_mutex.
std::int64_t last_valid_frame_ms;
bool seen_valid_frame;
std::int64_t last_reset_ms;
std::int64_t reset_interval_ms = first_reset_interval_ms;
std::uint32_t resets_since_valid;

/*
 * nRF UARTE EasyDMA keeps receiving while Bluetooth radio interrupts preempt
 * the RS485 manager thread. The former uart_poll_in() loop had only one byte
 * time of margin at 460800 baud and lost bytes during BLE connection and scan
 * events.
 */
inline constexpr std::size_t dma_buffer_size = 256;
inline constexpr std::size_t receive_ring_size = 2048;
inline constexpr std::int32_t receive_idle_timeout_us = 100;

alignas(
    4) std::array<std::array<std::uint8_t, dma_buffer_size>, 2> dma_buffers{};
std::array<bool, 2> dma_buffer_in_use{};
std::array<std::uint8_t, receive_ring_size> receive_ring{};
std::size_t receive_read{};
std::size_t receive_write{};
std::size_t receive_count{};
k_spinlock receive_lock;
K_SEM_DEFINE(receive_ready, 0, 1);
K_SEM_DEFINE(transmit_complete, 0, 1);
atomic_t transmit_result;

void note_receive_fault() { atomic_inc(&error_count); }

void ring_put(const std::uint8_t *bytes, std::size_t length) {
  if (bytes == nullptr || length == 0U) {
    return;
  }
  bool stored = false;
  bool overflowed = false;
  const k_spinlock_key_t key = k_spin_lock(&receive_lock);
  for (std::size_t index = 0; index < length; ++index) {
    if (receive_count == receive_ring.size()) {
      overflowed = true;
      break;
    }
    receive_ring[receive_write] = bytes[index];
    receive_write = (receive_write + 1U) % receive_ring.size();
    ++receive_count;
    stored = true;
  }
  k_spin_unlock(&receive_lock, key);
  if (overflowed) {
    note_receive_fault();
  }
  (void)atomic_add(&bytes_since_valid, static_cast<atomic_val_t>(length));
  if (stored) {
    k_sem_give(&receive_ready);
  }
}

bool ring_get(std::uint8_t &byte) {
  const k_spinlock_key_t key = k_spin_lock(&receive_lock);
  if (receive_count == 0U) {
    k_spin_unlock(&receive_lock, key);
    return false;
  }
  byte = receive_ring[receive_read];
  receive_read = (receive_read + 1U) % receive_ring.size();
  --receive_count;
  k_spin_unlock(&receive_lock, key);
  return true;
}

void clear_receive_ring() {
  const k_spinlock_key_t key = k_spin_lock(&receive_lock);
  receive_read = 0U;
  receive_write = 0U;
  receive_count = 0U;
  k_spin_unlock(&receive_lock, key);
  k_sem_reset(&receive_ready);
}

int start_receiver(const device *receiver) {
  dma_buffer_in_use = {true, false};
  const int error =
      uart_rx_enable(receiver, dma_buffers[0].data(), dma_buffers[0].size(),
                     receive_idle_timeout_us);
  if (error == 0) {
    atomic_set(&receiver_running, 1);
  }
  return error;
}

void uart_event_callback(const device *receiver, struct uart_event *event,
                         void *) {
  switch (event->type) {
  case UART_RX_RDY:
    ring_put(event->data.rx.buf + event->data.rx.offset, event->data.rx.len);
    break;

  case UART_RX_BUF_REQUEST: {
    // While the watchdog is stopping or kicking the receiver, offering a
    // second buffer would arm the ENDRX->STARTRX short and restart reception
    // behind the driver's back.
    if (atomic_get(&deliberate_stop) != 0) {
      break;
    }
    for (std::size_t index = 0; index < dma_buffers.size(); ++index) {
      if (dma_buffer_in_use[index]) {
        continue;
      }
      dma_buffer_in_use[index] = true;
      if (uart_rx_buf_rsp(receiver, dma_buffers[index].data(),
                          dma_buffers[index].size()) != 0) {
        dma_buffer_in_use[index] = false;
        note_receive_fault();
      }
      break;
    }
    break;
  }

  case UART_RX_BUF_RELEASED:
    for (std::size_t index = 0; index < dma_buffers.size(); ++index) {
      if (event->data.rx_buf.buf == dma_buffers[index].data()) {
        dma_buffer_in_use[index] = false;
        break;
      }
    }
    break;

  case UART_RX_STOPPED:
    note_receive_fault();
    atomic_inc(&receive_stop_count);
    atomic_inc(&stops_since_valid);
    atomic_set(&stop_pending_since_ms,
               static_cast<atomic_val_t>(k_uptime_get_32()));
    atomic_set(&stop_pending, 1);

    hub_debug::log("RS485_RX_STOPPED reason=0x%08x\r\n",
                      static_cast<unsigned>(event->data.rx_stop.reason));

    break;

  case UART_RX_DISABLED: {
    atomic_set(&receiver_running, 0);
    atomic_set(&stop_pending, 0);
    if (atomic_get(&deliberate_stop) != 0) {
      // The watchdog stopped reception and restarts it itself.
      k_sem_give(&receiver_disabled);
      break;
    }
    const int error = start_receiver(receiver);
    if (error != 0) {
      note_receive_fault();
      atomic_inc(&receive_restart_failure_count);
      hub_debug::log("RS485_RX_RESTART_FAILED err=%d\r\n", error);
    }
    break;
  }

  case UART_TX_DONE:
    // The final stop bit has left UARTE. Release the half-duplex bus
    // immediately so a node can start its response without waiting for
    // the RS485 manager thread to be scheduled.
    nrf_gpio_pin_clear(RS485_DIR_PIN);
    atomic_set(&transmit_result, 0);
    k_sem_give(&transmit_complete);
    break;

  case UART_TX_ABORTED:
    nrf_gpio_pin_clear(RS485_DIR_PIN);
    atomic_set(&transmit_result, -EIO);
    k_sem_give(&transmit_complete);
    break;
  }
}

/*
 * Read-only snapshot of the UARTE and its RX pin, logged before a reset so a
 * stuck receiver can be diagnosed afterwards. Reading these registers has no
 * side effects (ERRORSRC clears only on write).
 */
void log_receiver_snapshot(const char *when) {
  auto *const regs =
      reinterpret_cast<NRF_UARTE_Type *>(DT_REG_ADDR(UART_NODE));
  const std::uint32_t psel_rxd = regs->PSEL.RXD;
  const bool rx_connected = (psel_rxd & 0x80000000U) == 0U;
  const std::uint32_t rx_pin = psel_rxd & 0x1FU;
  const std::uint32_t rx_port = (psel_rxd >> 5U) & 0x1U;
  std::uint32_t rx_pin_cnf = 0xFFFFFFFFU;
  std::uint32_t rx_pin_level = 0xFFFFFFFFU;
  if (rx_connected) {
    NRF_GPIO_Type *const port = rx_port == 0U ? NRF_P0 : NRF_P1;
    rx_pin_cnf = port->PIN_CNF[rx_pin];
    rx_pin_level = (port->IN >> rx_pin) & 0x1U;
  }
  hub_debug::log(
      "RS485_RX_SNAPSHOT when=%s running=%d enable=%u errorsrc=0x%02x "
      "baud=0x%08x inten=0x%08x rxd_amount=%u rx_pin=P%u.%02u%s "
      "pin_cnf=0x%08x pin_in=%u bytes_since_valid=%d stops_since_valid=%d\r\n",
      when, static_cast<int>(atomic_get(&receiver_running)),
      static_cast<unsigned>(regs->ENABLE),
      static_cast<unsigned>(regs->ERRORSRC),
      static_cast<unsigned>(regs->BAUDRATE),
      static_cast<unsigned>(regs->INTENSET),
      static_cast<unsigned>(regs->RXD.AMOUNT), static_cast<unsigned>(rx_port),
      static_cast<unsigned>(rx_pin), rx_connected ? "" : "(disconnected)",
      static_cast<unsigned>(rx_pin_cnf), static_cast<unsigned>(rx_pin_level),
      static_cast<int>(atomic_get(&bytes_since_valid)),
      static_cast<int>(atomic_get(&stops_since_valid)));

  // Pending hardware events and interrupt state. A stop that never completes
  // shows here: rxto=1 with the IRQ not pending means the driver saw and
  // discarded it; rxto=0 means the hardware never produced it.
  const auto irq = static_cast<IRQn_Type>(DT_IRQN(UART_NODE));
  hub_debug::log(
      "RS485_RX_EVENTS when=%s rxstarted=%u endrx=%u rxto=%u error=%u "
      "rxdrdy=%u txstopped=%u irq_enabled=%u irq_pending=%u rxd_ptr=0x%08x "
      "rxd_maxcnt=%u\r\n",
      when, static_cast<unsigned>(regs->EVENTS_RXSTARTED),
      static_cast<unsigned>(regs->EVENTS_ENDRX),
      static_cast<unsigned>(regs->EVENTS_RXTO),
      static_cast<unsigned>(regs->EVENTS_ERROR),
      static_cast<unsigned>(regs->EVENTS_RXDRDY),
      static_cast<unsigned>(regs->EVENTS_TXSTOPPED),
      static_cast<unsigned>(NVIC_GetEnableIRQ(irq)),
      static_cast<unsigned>(NVIC_GetPendingIRQ(irq)),
      static_cast<unsigned>(regs->RXD.PTR),
      static_cast<unsigned>(regs->RXD.MAXCNT));
}

/*
 * Kick a wedged receiver.
 *
 * Observed failure: after a burst of framing errors the UARTE receiver has
 * stopped in hardware (no RXDRDY although bytes reach the RX pin), but the
 * driver never processed the RXTO that marks the stop. It then waits for an
 * RXTO forever, so uart_rx_disable() never completes and uart_rx_enable()
 * returns -EBUSY. A stopped receiver produces no RXTO when asked to stop
 * again, so the driver cannot get out by itself.
 *
 * Start the receiver in hardware (it still points at the driver's own DMA
 * buffer) and stop it straight away. A running receiver answers STOPRX with
 * ENDRX and RXTO; the driver, which is already expecting RXTO after the
 * uart_rx_disable() call, finishes its stop and delivers RX_DISABLED.
 * deliberate_stop must be set, so the RX_BUF_REQUEST that STARTRX raises is
 * ignored and the ENDRX->STARTRX short stays off.
 */
void kick_receiver() {
  auto *const regs =
      reinterpret_cast<NRF_UARTE_Type *>(DT_REG_ADDR(UART_NODE));
  nrf_uarte_shorts_disable(regs, NRF_UARTE_SHORT_ENDRX_STARTRX);
  nrf_uarte_task_trigger(regs, NRF_UARTE_TASK_STARTRX);
  k_busy_wait(20);
  nrf_uarte_task_trigger(regs, NRF_UARTE_TASK_STOPRX);
}

/*
 * Stop reception, wait for the driver to confirm, then start it again. If
 * the driver never confirms the stop, kick the receiver so it can, then
 * start again. Verbose resets (the silence watchdog) log register snapshots;
 * quiet ones (the wedge check, which can fire often during noisy audio) log
 * one line. Caller holds bus_mutex.
 */
void reset_receiver(bool verbose, const char *reason) {
  atomic_inc(&receiver_reset_count);
  if (verbose) {
    log_receiver_snapshot("before_reset");
  }

  k_sem_reset(&receiver_disabled);
  atomic_set(&deliberate_stop, 1);
  const int disable_error = uart_rx_disable(uart);
  int wait_error = 0;
  int kick_wait_error = 1; // 1 = no kick needed
  if (disable_error == 0) {
    wait_error = k_sem_take(&receiver_disabled, K_MSEC(rx_disable_wait_ms));
    if (wait_error != 0) {
      atomic_inc(&receiver_kick_count);
      kick_receiver();
      kick_wait_error =
          k_sem_take(&receiver_disabled, K_MSEC(rx_disable_wait_ms));
      if (verbose) {
        log_receiver_snapshot("after_kick");
      }
    }
  }
  atomic_set(&deliberate_stop, 0);
  // Whatever happened, this stop is dealt with. If the restart below fails,
  // the silence watchdog takes over rather than the wedge check repeating.
  atomic_set(&stop_pending, 0);

  // -EFAULT from uart_rx_disable() means the driver had no receive buffer,
  // so it already considered reception stopped.
  clear_receive_ring();
  const int start_error = start_receiver(uart);
  if (start_error != 0) {
    atomic_inc(&receive_restart_failure_count);
  }
  hub_debug::log("RS485_RX_RESET reason=%s disable_err=%d wait_err=%d "
                 "kick_wait_err=%d start_err=%d\r\n",
                 reason, disable_error, wait_error, kick_wait_error,
                 start_error);
  if (verbose) {
    log_receiver_snapshot("after_reset");
  }
}

/*
 * Fast check for the known wedge: an RX_STOPPED whose RX_DISABLED never
 * arrived. Runs before every exchange attempt, so a wedge costs one attempt
 * rather than a second of silence. Caller holds bus_mutex.
 */
void check_receiver_wedge() {
  if (atomic_get(&stop_pending) == 0) {
    return;
  }
  const std::uint32_t since =
      static_cast<std::uint32_t>(atomic_get(&stop_pending_since_ms));
  const std::uint32_t age_ms = k_uptime_get_32() - since;
  if (age_ms < wedge_after_ms) {
    return;
  }
  atomic_inc(&receiver_wedge_count);
  hub_debug::log("RS485_RX_WEDGED stop_age_ms=%u\r\n",
                 static_cast<unsigned>(age_ms));
  reset_receiver(false, "wedge");
}

void note_valid_frame() {
  const std::int64_t now = k_uptime_get();
  if (resets_since_valid != 0U) {
    atomic_inc(&receiver_recovery_count);
    hub_debug::log("RS485_RX_RECOVERED silent_ms=%d resets=%u\r\n",
                   static_cast<int>(now - last_valid_frame_ms),
                   static_cast<unsigned>(resets_since_valid));
  }
  last_valid_frame_ms = now;
  seen_valid_frame = true;
  resets_since_valid = 0U;
  reset_interval_ms = first_reset_interval_ms;
  atomic_clear(&bytes_since_valid);
  atomic_clear(&stops_since_valid);
}

// Caller holds bus_mutex.
void check_receiver_watchdog() {
  if (!seen_valid_frame) {
    return; // Nothing has ever answered, so silence is not a fault.
  }
  const std::int64_t now = k_uptime_get();
  const std::int64_t silent_ms = now - last_valid_frame_ms;
  if (silent_ms < stuck_after_ms || silent_ms > watchdog_active_for_ms) {
    return;
  }
  if (resets_since_valid != 0U && now - last_reset_ms < reset_interval_ms) {
    return;
  }
  hub_debug::log("RS485_RX_STUCK silent_ms=%d attempt=%u\r\n",
                 static_cast<int>(silent_ms),
                 static_cast<unsigned>(resets_since_valid + 1U));
  reset_receiver(true, "silence");
  last_reset_ms = k_uptime_get();
  ++resets_since_valid;
  reset_interval_ms = std::min(reset_interval_ms * 2, max_reset_interval_ms);
}

bool receive_byte_until(std::int64_t deadline, std::uint8_t &byte) {
  while (true) {
    if (ring_get(byte)) {
      return true;
    }
    const std::int64_t remaining = deadline - k_uptime_get();
    if (remaining < 0) {
      return false;
    }
    if (k_sem_take(&receive_ready,
                   K_MSEC(std::max<std::int64_t>(1, remaining))) != 0) {
      return ring_get(byte);
    }
  }
}

bool transmit_encoded(const std::uint8_t *bytes, std::size_t length) {
  k_sem_reset(&transmit_complete);
  atomic_set(&transmit_result, -EINPROGRESS);

  // U3 has /RE and DE tied together on RS485_DIR:
  //   LOW  -> transmitter disabled, receiver enabled
  //   HIGH -> transmitter enabled, receiver disabled
  nrf_gpio_pin_set(RS485_DIR_PIN);

  const int error = uart_tx(uart, bytes, length, SYS_FOREVER_US);
  if (error != 0) {
    nrf_gpio_pin_clear(RS485_DIR_PIN);
    return false;
  }
  if (k_sem_take(&transmit_complete, K_MSEC(20)) != 0) {
    (void)uart_tx_abort(uart);
    (void)k_sem_take(&transmit_complete, K_MSEC(20));
    nrf_gpio_pin_clear(RS485_DIR_PIN);
    return false;
  }
  return atomic_get(&transmit_result) == 0;
}

std::uint16_t allocate_transfer() {
  const atomic_val_t raw = atomic_inc(&next_transfer) + 1;
  const std::uint16_t value = static_cast<std::uint16_t>(raw);
  if (value == 0U) {
    atomic_set(&next_transfer, 1);
    return 1;
  }
  return value;
}

void drain_input() { clear_receive_ring(); }

bool transmit_message(std::uint8_t destination,
                      rig::transport::Channel channel,
                      std::uint16_t transfer_id, std::uint8_t flags,
                      const std::uint8_t *payload, std::size_t payload_length) {
  constexpr std::size_t fragment_payload = 200;
  const std::size_t count =
      payload_length == 0U
          ? 1U
          : (payload_length + fragment_payload - 1U) / fragment_payload;
  if (count > 255U || (payload_length != 0U && payload == nullptr)) {
    return false;
  }

  for (std::size_t index = 0; index < count; ++index) {
    const std::size_t offset = index * fragment_payload;
    const std::size_t chunk =
        payload_length == 0U
            ? 0U
            : std::min(fragment_payload, payload_length - offset);
    rig::transport::Header header;
    header.channel = channel;
    header.flags = flags;
    if (index == 0U) {
      header.flags |= rig::transport::first_fragment;
    }
    if (index + 1U == count) {
      header.flags |= rig::transport::last_fragment;
    }
    header.source = rig::config::hub_address;
    header.destination = destination;
    header.fragment_index = static_cast<std::uint8_t>(index);
    header.fragment_count = static_cast<std::uint8_t>(count);
    header.transfer_id = transfer_id;

    std::uint8_t raw[rig::transport::max_raw_frame_size]{};
    const std::size_t raw_length = rig::transport::encode_frame(
        header, chunk == 0U ? nullptr : payload + offset, chunk, raw,
        sizeof(raw));
    std::uint8_t encoded[rig::transport::max_encoded_uart_frame]{};
    const std::size_t encoded_length = rig::transport::cobs_encode(
        raw, raw_length, encoded, sizeof(encoded));
    if (raw_length == 0U || encoded_length == 0U) {
      return false;
    }
    if (!transmit_encoded(encoded, encoded_length)) {
      return false;
    }
  }
  k_busy_wait(30);
  return true;
}

int receive_message(std::uint16_t transfer_id, std::uint32_t timeout_ms,
                    Received &response, std::uint8_t expected_source = 0xFFU) {
  rig::transport::UartDecoder decoder;
  rig::transport::Reassembler reassembler;
  const std::int64_t deadline = k_uptime_get() + timeout_ms;
  while (k_uptime_get() <= deadline) {
    std::uint8_t byte;
    if (!receive_byte_until(deadline, byte)) {
      break;
    }
    rig::transport::FrameView frame;
    const auto decoded = decoder.push(byte, frame);
    if (decoded == rig::transport::DecodeResult::incomplete) {
      continue;
    }
    if (decoded != rig::transport::DecodeResult::ok) {
      atomic_inc(&error_count);
      atomic_inc(&decode_error_count);
      continue;
    }
    rig::transport::MessageView message;
    const auto complete = reassembler.accept(frame, message);
    if (complete == rig::transport::DecodeResult::incomplete) {
      continue;
    }
    if (complete != rig::transport::DecodeResult::ok) {
      atomic_inc(&error_count);
      atomic_inc(&reassembly_error_count);
      continue;
    }
    // Any intact frame proves the receive path works, even a stale one.
    note_valid_frame();
    if (message.transfer_id != transfer_id ||
        message.destination != rig::config::hub_address ||
        (expected_source != 0xFFU && message.source != expected_source)) {
      continue;
    }
    response.channel = message.channel;
    response.flags = message.flags;
    response.source = message.source;
    response.destination = message.destination;
    response.transfer_id = message.transfer_id;
    response.length = static_cast<std::uint16_t>(message.payload_length);
    if (message.payload_length != 0U) {
      std::memcpy(response.payload.data(), message.payload,
                  message.payload_length);
    }
    return 0;
  }
  return -ETIMEDOUT;
}

} // namespace

int initialize() {
  if (!device_is_ready(uart)) {
    return -ENODEV;
  }

  // Default to listening. The pin is asserted only while UARTE is sending.
  nrf_gpio_cfg_output(RS485_DIR_PIN);
  nrf_gpio_pin_clear(RS485_DIR_PIN);

  atomic_clear(&error_count);
  atomic_clear(&retry_count);
  atomic_clear(&timeout_count);
  atomic_clear(&decode_error_count);
  atomic_clear(&reassembly_error_count);
  atomic_clear(&transmit_error_count);
  atomic_clear(&receive_stop_count);
  atomic_clear(&receive_restart_failure_count);
  atomic_clear(&receiver_reset_count);
  atomic_clear(&receiver_recovery_count);
  atomic_clear(&receiver_kick_count);
  atomic_clear(&receiver_wedge_count);
  atomic_clear(&stop_pending);
  atomic_clear(&receiver_running);
  atomic_clear(&deliberate_stop);
  atomic_clear(&bytes_since_valid);
  atomic_clear(&stops_since_valid);
  atomic_clear(&transmit_result);
  clear_receive_ring();
  int error = uart_callback_set(uart, uart_event_callback, nullptr);
  if (error != 0) {
    return error;
  }
  error = start_receiver(uart);
  return error == -EBUSY ? 0 : error;
}

int exchange(std::uint8_t destination, rig::transport::Channel channel,
             const std::uint8_t *payload, std::size_t payload_length,
             Received &response, std::uint32_t timeout_ms, std::uint8_t flags,
             bool acknowledge_response) {
  const std::uint16_t transfer = allocate_transfer();
  k_mutex_lock(&bus_mutex, K_FOREVER);
  check_receiver_watchdog();
  drain_input();
  int result = -ETIMEDOUT;
  for (unsigned int attempt = 0; attempt < 2; ++attempt) {
    if (attempt != 0U) {
      atomic_inc(&retry_count);
    }
    // Before every attempt, so a wedge during the first attempt's reply is
    // cleared before the retry is sent.
    check_receiver_wedge();
    if (!transmit_message(destination, channel, transfer, flags, payload,
                          payload_length)) {
      atomic_inc(&transmit_error_count);
      result = -EIO;
      continue;
    }
    result = receive_message(transfer, timeout_ms, response, destination);
    if (result == 0 && acknowledge_response) {
      if (!transmit_message(destination, channel, transfer,
                            rig::transport::acknowledgement, nullptr, 0)) {
        atomic_inc(&transmit_error_count);
        result = -EIO;
      }
    }
    if (result == 0) {
      break;
    }
  }
  if (result != 0) {
    if (result == -ETIMEDOUT) {
      atomic_inc(&timeout_count);
    }
    atomic_inc(&error_count);
  }
  k_mutex_unlock(&bus_mutex);
  return result;
}

int send_one_way(std::uint8_t destination, rig::transport::Channel channel,
                 std::uint16_t transfer_id, std::uint8_t flags,
                 const std::uint8_t *payload, std::size_t payload_length) {
  k_mutex_lock(&bus_mutex, K_FOREVER);
  drain_input();
  const bool result = transmit_message(destination, channel, transfer_id, flags,
                                       payload, payload_length);
  k_mutex_unlock(&bus_mutex);
  if (!result) {
    atomic_inc(&transmit_error_count);
    atomic_inc(&error_count);
    return -EIO;
  }
  return 0;
}

std::uint32_t errors() {
  return static_cast<std::uint32_t>(atomic_get(&error_count));
}

std::uint32_t retries() {
  return static_cast<std::uint32_t>(atomic_get(&retry_count));
}

Diagnostics diagnostics() {
  return Diagnostics{
      static_cast<std::uint32_t>(atomic_get(&error_count)),
      static_cast<std::uint32_t>(atomic_get(&retry_count)),
      static_cast<std::uint32_t>(atomic_get(&timeout_count)),
      static_cast<std::uint32_t>(atomic_get(&decode_error_count)),
      static_cast<std::uint32_t>(atomic_get(&reassembly_error_count)),
      static_cast<std::uint32_t>(atomic_get(&transmit_error_count)),
      static_cast<std::uint32_t>(atomic_get(&receive_stop_count)),
      static_cast<std::uint32_t>(atomic_get(&receive_restart_failure_count)),
      static_cast<std::uint32_t>(atomic_get(&receiver_reset_count)),
      static_cast<std::uint32_t>(atomic_get(&receiver_recovery_count)),
      static_cast<std::uint32_t>(atomic_get(&receiver_kick_count)),
      static_cast<std::uint32_t>(atomic_get(&receiver_wedge_count)),
      atomic_get(&receiver_running) != 0,
  };
}

} // namespace hub_rs485
