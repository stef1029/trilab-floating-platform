#include "rs485_bus.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "debug_log.hpp"
#include "fairy_shared/bytes.hpp"
#include "fairy_shared/system_config.hpp"
#include "timebase.hpp"

namespace korora_rs485 {
namespace {

#define UART_NODE DT_NODELABEL(uart1)

const device *uart = DEVICE_DT_GET(UART_NODE);
K_MUTEX_DEFINE(bus_mutex);
atomic_t next_transfer = ATOMIC_INIT(1);
atomic_t error_count;
atomic_t retry_count;
atomic_t timeout_count;
atomic_t decode_error_count;
atomic_t reassembly_error_count;
atomic_t transmit_error_count;

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
  return uart_rx_enable(receiver, dma_buffers[0].data(), dma_buffers[0].size(),
                        receive_idle_timeout_us);
}

void uart_event_callback(const device *receiver, struct uart_event *event,
                         void *) {
  switch (event->type) {
  case UART_RX_RDY:
    ring_put(event->data.rx.buf + event->data.rx.offset, event->data.rx.len);
    break;

  case UART_RX_BUF_REQUEST: {
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
    break;

  case UART_RX_DISABLED:
    if (start_receiver(receiver) != 0) {
      note_receive_fault();
    }
    break;

  case UART_TX_DONE:
    atomic_set(&transmit_result, 0);
    k_sem_give(&transmit_complete);
    break;

  case UART_TX_ABORTED:
    atomic_set(&transmit_result, -EIO);
    k_sem_give(&transmit_complete);
    break;
  }
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
  const int error = uart_tx(uart, bytes, length, SYS_FOREVER_US);
  if (error != 0) {
    return false;
  }
  if (k_sem_take(&transmit_complete, K_MSEC(20)) != 0) {
    (void)uart_tx_abort(uart);
    (void)k_sem_take(&transmit_complete, K_MSEC(20));
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
                      fairy::transport::Channel channel,
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
    fairy::transport::Header header;
    header.channel = channel;
    header.flags = flags;
    if (index == 0U) {
      header.flags |= fairy::transport::first_fragment;
    }
    if (index + 1U == count) {
      header.flags |= fairy::transport::last_fragment;
    }
    header.source = fairy::config::korora_address;
    header.destination = destination;
    header.fragment_index = static_cast<std::uint8_t>(index);
    header.fragment_count = static_cast<std::uint8_t>(count);
    header.transfer_id = transfer_id;

    std::uint8_t raw[fairy::transport::max_raw_frame_size]{};
    const std::size_t raw_length = fairy::transport::encode_frame(
        header, chunk == 0U ? nullptr : payload + offset, chunk, raw,
        sizeof(raw));
    std::uint8_t encoded[fairy::transport::max_encoded_uart_frame]{};
    const std::size_t encoded_length = fairy::transport::cobs_encode(
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
  fairy::transport::UartDecoder decoder;
  fairy::transport::Reassembler reassembler;
  const std::int64_t deadline = k_uptime_get() + timeout_ms;
  while (k_uptime_get() <= deadline) {
    std::uint8_t byte;
    if (!receive_byte_until(deadline, byte)) {
      break;
    }
    fairy::transport::FrameView frame;
    const auto decoded = decoder.push(byte, frame);
    if (decoded == fairy::transport::DecodeResult::incomplete) {
      continue;
    }
    if (decoded != fairy::transport::DecodeResult::ok) {
      atomic_inc(&error_count);
      atomic_inc(&decode_error_count);
      continue;
    }
    fairy::transport::MessageView message;
    const auto complete = reassembler.accept(frame, message);
    if (complete == fairy::transport::DecodeResult::incomplete) {
      continue;
    }
    if (complete != fairy::transport::DecodeResult::ok) {
      atomic_inc(&error_count);
      atomic_inc(&reassembly_error_count);
      continue;
    }
    if (message.transfer_id != transfer_id ||
        message.destination != fairy::config::korora_address ||
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
  atomic_clear(&error_count);
  atomic_clear(&retry_count);
  atomic_clear(&timeout_count);
  atomic_clear(&decode_error_count);
  atomic_clear(&reassembly_error_count);
  atomic_clear(&transmit_error_count);
  atomic_clear(&transmit_result);
  clear_receive_ring();
  int error = uart_callback_set(uart, uart_event_callback, nullptr);
  if (error != 0) {
    return error;
  }
  error = start_receiver(uart);
  return error == -EBUSY ? 0 : error;
}

int exchange(std::uint8_t destination, fairy::transport::Channel channel,
             const std::uint8_t *payload, std::size_t payload_length,
             Received &response, std::uint32_t timeout_ms, std::uint8_t flags,
             bool acknowledge_response) {
  const std::uint16_t transfer = allocate_transfer();
  k_mutex_lock(&bus_mutex, K_FOREVER);
  drain_input();
  int result = -ETIMEDOUT;
  for (unsigned int attempt = 0; attempt < 2; ++attempt) {
    if (attempt != 0U) {
      atomic_inc(&retry_count);
    }
    if (!transmit_message(destination, channel, transfer, flags, payload,
                          payload_length)) {
      atomic_inc(&transmit_error_count);
      result = -EIO;
      continue;
    }
    result = receive_message(transfer, timeout_ms, response, destination);
    if (result == 0 && acknowledge_response) {
      if (!transmit_message(destination, channel, transfer,
                            fairy::transport::acknowledgement, nullptr, 0)) {
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

int send_one_way(std::uint8_t destination, fairy::transport::Channel channel,
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

std::size_t discover(std::uint32_t nonce, std::uint8_t round,
                     DiscoveredOffer *offers, std::size_t capacity) {
  if (offers == nullptr || capacity == 0U) {
    return 0;
  }
  std::uint8_t parameters[4]{};
  parameters[0] =
      static_cast<std::uint8_t>(fairy::protocol::discovery_slot_count);
  fairy::wire::put_u16(parameters + 1, fairy::protocol::discovery_slot_us);
  parameters[3] = round;
  fairy::protocol::MagellanHeader header;
  header.type = fairy::protocol::MagellanType::discover;
  header.nonce = nonce;
  std::uint8_t application[32]{};
  const std::size_t application_length = fairy::protocol::encode_magellan(
      header, parameters, sizeof(parameters), application, sizeof(application));
  const std::uint16_t transfer = allocate_transfer();

  k_mutex_lock(&bus_mutex, K_FOREVER);
  drain_input();
  if (!transmit_message(fairy::config::broadcast_address,
                        fairy::transport::Channel::magellan, transfer,
                        fairy::transport::ack_required, application,
                        application_length)) {
    k_mutex_unlock(&bus_mutex);
    return 0;
  }

  fairy::transport::UartDecoder decoder;
  const std::int64_t window_ms = (fairy::protocol::discovery_slot_count *
                                      fairy::protocol::discovery_slot_us +
                                  3000U) /
                                 1000U;
  const std::int64_t deadline = k_uptime_get() + window_ms;
  std::size_t count = 0;
  while (k_uptime_get() <= deadline) {
    std::uint8_t byte;
    if (!receive_byte_until(deadline, byte)) {
      break;
    }
    fairy::transport::FrameView frame;
    if (decoder.push(byte, frame) != fairy::transport::DecodeResult::ok) {
      continue;
    }
    if (frame.header.fragment_count != 1U ||
        frame.header.channel != fairy::transport::Channel::magellan ||
        frame.header.destination != fairy::config::korora_address) {
      continue;
    }
    fairy::protocol::MagellanView message;
    if (!fairy::protocol::decode_magellan(frame.payload, frame.payload_length,
                                          message) ||
        message.header.type != fairy::protocol::MagellanType::offer ||
        message.header.nonce != nonce) {
      continue;
    }
    fairy::protocol::Offer offer;
    if (!fairy::protocol::decode_offer(message.payload,
                                       message.header.payload_length, offer)) {
      continue;
    }
    bool duplicate = false;
    for (std::size_t i = 0; i < count; ++i) {
      duplicate = duplicate || offers[i].offer.uuid == offer.uuid;
    }
    if (!duplicate && count < capacity) {
      offers[count++] = DiscoveredOffer{offer, frame.header.transfer_id};
    }
  }
  k_mutex_unlock(&bus_mutex);
  return count;
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
  };
}

} // namespace korora_rs485
