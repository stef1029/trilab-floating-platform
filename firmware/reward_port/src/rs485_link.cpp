#include "rs485_link.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include <stm32g0xx_hal.h>

#include "application.hpp"
#include "board_profile.hpp"
#include "debug_log.hpp"
#include "rig_protocol/bytes.hpp"
#include "rig_protocol/record_protocol.hpp"
#include "rig_protocol/discovery_protocol.hpp"
#include "rig_protocol/node_identity.hpp"
#include "rig_protocol/system_config.hpp"
#include "rig_protocol/transport.hpp"
#include "outputs.hpp"
#include "record_store.hpp"
#include "timebase.hpp"

#ifndef UID_BASE
#define UID_BASE 0x1FFF7590UL
#endif

namespace node_rs485 {
namespace {

UART_HandleTypeDef uart;
std::uint8_t receive_byte;
std::array<std::uint8_t, 512> receive_ring;
volatile std::uint16_t receive_read;
volatile std::uint16_t receive_write;

rig::transport::UartDecoder decoder;
rig::transport::Reassembler reassembler;
rig::protocol::DeviceUuid device_uuid;
/* Fixed from the identity page at boot. A board without a valid identity
 * keeps the unassigned address and never transmits, so it cannot collide
 * with a numbered board. */
bool identity_valid;
std::uint8_t local_address;
std::uint8_t local_logical_slot = 0xFF;
std::uint64_t next_missing_identity_blink;
inline constexpr std::uint64_t missing_identity_blink_ticks = 16'000'000ULL;
inline constexpr std::uint32_t missing_identity_blink_ms = 200U;
volatile std::uint32_t error_count;
std::uint32_t duplicate_count;
std::uint16_t last_request_transfer;
rig::transport::Channel last_request_channel =
    rig::transport::Channel::record;
bool last_request_valid;
std::uint16_t outstanding_record_transfer;
bool outstanding_record;
std::uint64_t last_contact;

void uart_initialize() {
  __HAL_RCC_USART1_CLK_ENABLE();
  uart.Instance = USART1;
  uart.Init.BaudRate = rig::config::rs485_baud;
  uart.Init.WordLength = UART_WORDLENGTH_8B;
  uart.Init.StopBits = UART_STOPBITS_1;
  uart.Init.Parity = UART_PARITY_NONE;
  uart.Init.Mode = UART_MODE_TX_RX;
  uart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  uart.Init.OverSampling = UART_OVERSAMPLING_16;
  uart.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  uart.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&uart) != HAL_OK) {
    Error_Handler();
  }
  /* Byte reception must pre-empt DAC bookkeeping and other peripherals. */
  HAL_NVIC_SetPriority(USART1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(USART1_IRQn);
  if (HAL_UART_Receive_IT(&uart, &receive_byte, 1) != HAL_OK) {
    Error_Handler();
  }
}

bool transmit_raw(const std::uint8_t *raw, std::size_t raw_length) {
  std::uint8_t encoded[rig::transport::max_encoded_uart_frame]{};
  const std::size_t encoded_length =
      rig::transport::cobs_encode(raw, raw_length, encoded, sizeof(encoded));
  if (encoded_length == 0U) {
    ++error_count;
    return false;
  }

  node_timebase::busy_wait_us(40);
  node_board::set_rs485_transmit(true);
  node_timebase::busy_wait_us(1);
  const HAL_StatusTypeDef status = HAL_UART_Transmit(
      &uart, encoded, static_cast<std::uint16_t>(encoded_length), 20);
  node_board::set_rs485_transmit(false);
  if (status != HAL_OK) {
    ++error_count;
    return false;
  }
  return true;
}

bool send_message(rig::transport::Channel channel, std::uint8_t flags,
                  std::uint8_t source, std::uint8_t destination,
                  std::uint16_t transfer_id, const std::uint8_t *message,
                  std::size_t length) {
  constexpr std::size_t fragment_payload = 200;
  const std::size_t fragment_count =
      length == 0U ? 1U : (length + fragment_payload - 1U) / fragment_payload;
  if (fragment_count > 255U) {
    return false;
  }

  for (std::size_t index = 0; index < fragment_count; ++index) {
    const std::size_t offset = index * fragment_payload;
    const std::size_t chunk =
        length == 0U ? 0U : std::min(fragment_payload, length - offset);
    rig::transport::Header header;
    header.channel = channel;
    header.flags = flags;
    if (index == 0U) {
      header.flags |= rig::transport::first_fragment;
    }
    if (index + 1U == fragment_count) {
      header.flags |= rig::transport::last_fragment;
    }
    header.source = source;
    header.destination = destination;
    header.fragment_index = static_cast<std::uint8_t>(index);
    header.fragment_count = static_cast<std::uint8_t>(fragment_count);
    header.transfer_id = transfer_id;

    std::uint8_t raw[rig::transport::max_raw_frame_size]{};
    const std::size_t raw_length = rig::transport::encode_frame(
        header, length == 0U ? nullptr : message + offset, chunk, raw,
        sizeof(raw));
    if (raw_length == 0U || !transmit_raw(raw, raw_length)) {
      return false;
    }
  }
  return true;
}

void send_poll_response(std::uint16_t transfer_id) {
  const node_records::StoredRecord *record = node_records::front();
  if (record == nullptr) {
    (void)send_message(rig::transport::Channel::record,
                       rig::transport::response, local_address,
                       rig::config::hub_address, transfer_id, nullptr, 0);
    return;
  }
  if (send_message(rig::transport::Channel::record, rig::transport::response,
                   local_address, rig::config::hub_address, transfer_id,
                   record->bytes.data(), record->length)) {
    outstanding_record = true;
    outstanding_record_transfer = transfer_id;
  }
}

/*
 * The node's address is fixed by its identity page, so the only discovery
 * message it still acts on is the hub's boot-time release, which now just
 * means "the hub restarted: make outputs safe". Discover and assign are
 * ignored; the address never changes.
 */
void handle_discovery(const rig::transport::MessageView &message) {
  rig::protocol::DiscoveryView view;
  if (!rig::protocol::decode_discovery(message.payload, message.payload_length,
                                        view)) {
    ++error_count;
    return;
  }
  if (view.header.type == rig::protocol::DiscoveryType::release &&
      view.header.payload_length == 0U) {
    outstanding_record = false;
    node_outputs::all_safe(node_timebase::now());
  }
}

bool read_identity(std::uint8_t &node_id) {
  std::uint8_t record[rig::identity::record_size];
  std::memcpy(record,
              reinterpret_cast<const void *>(rig::identity::page_address),
              sizeof(record));
  return rig::identity::decode_identity(record, sizeof(record), node_id);
}

void handle_message(const rig::transport::MessageView &message) {
  if (!identity_valid) {
    return;
  }
  const bool addressed =
      message.destination == local_address ||
      message.destination == rig::config::broadcast_address;
  if (!addressed || message.source != rig::config::hub_address) {
    return;
  }
  last_contact = node_timebase::now();

  const bool is_acknowledgement =
      (message.flags & rig::transport::acknowledgement) != 0U;
  if (!is_acknowledgement) {
    if (last_request_valid && message.transfer_id == last_request_transfer &&
        message.channel == last_request_channel) {
      ++duplicate_count;
    }
    last_request_valid = true;
    last_request_transfer = message.transfer_id;
    last_request_channel = message.channel;
  }

  if (message.channel == rig::transport::Channel::discovery) {
    handle_discovery(message);
    return;
  }

  if (message.channel == rig::transport::Channel::record) {
    if ((message.flags & rig::transport::acknowledgement) != 0U) {
      if (outstanding_record &&
          outstanding_record_transfer == message.transfer_id) {
        (void)node_records::pop();
        outstanding_record = false;
      }
      return;
    }
    if (message.payload_length == 0U) {
      send_poll_response(message.transfer_id);
    }
    return;
  }

  if (message.channel == rig::transport::Channel::command) {
    std::uint8_t response[rig::protocol::command_max_message_size]{};
    const std::size_t response_length = node_application::handle_host(
        message.payload, message.payload_length, response, sizeof(response));
    if (response_length != 0U) {
      (void)send_message(rig::transport::Channel::command,
                         rig::transport::response, local_address,
                         rig::config::hub_address, message.transfer_id,
                         response, response_length);
    }
  }
}

void consume_received() {
  while (receive_read != receive_write) {
    const std::uint8_t byte = receive_ring[receive_read];
    receive_read =
        static_cast<std::uint16_t>((receive_read + 1U) % receive_ring.size());
    rig::transport::FrameView frame;
    const auto result = decoder.push(byte, frame);
    if (result == rig::transport::DecodeResult::ok) {
      rig::transport::MessageView message;
      const auto reassembly = reassembler.accept(frame, message);
      if (reassembly == rig::transport::DecodeResult::ok) {
        handle_message(message);
      } else if (reassembly != rig::transport::DecodeResult::incomplete) {
        ++error_count;
      }
    } else if (result != rig::transport::DecodeResult::incomplete) {
      ++error_count;
    }
  }
}

} // namespace

void initialize() {
  local_address = rig::config::unassigned_address;
  local_logical_slot = 0xFF;
  last_contact = node_timebase::now();
  const volatile std::uint32_t *uid =
      reinterpret_cast<const volatile std::uint32_t *>(UID_BASE);
  for (std::size_t word = 0; word < 3; ++word) {
    const std::uint32_t value = uid[word];
    rig::wire::put_u32(device_uuid.data() + word * 4U, value);
  }

  std::uint8_t node_id = 0xFF;
  identity_valid = read_identity(node_id);
  if (identity_valid) {
    local_address = rig::config::node_address(node_id);
    local_logical_slot = node_id;
    node_debug::log("NODE_ID id=%u address=0x%02x\r\n",
                    static_cast<unsigned int>(node_id),
                    static_cast<unsigned int>(local_address));
  } else {
    node_debug::log("NODE_ID missing: flash with scripts/flash.py --id N; "
                    "RS485 stays silent\r\n");
  }
  uart_initialize();
}

void service() {
  consume_received();
  if (!identity_valid) {
    const std::uint64_t now_ticks = node_timebase::now();
    if (now_ticks >= next_missing_identity_blink) {
      node_outputs::set_rgb(255, 0, 0, missing_identity_blink_ms, now_ticks);
      next_missing_identity_blink = now_ticks + missing_identity_blink_ticks;
    }
  }
}

bool has_identity() { return identity_valid; }
const rig::protocol::DeviceUuid &uuid() { return device_uuid; }
std::uint8_t address() { return local_address; }
std::uint8_t logical_slot() { return local_logical_slot; }
std::uint32_t transport_errors() { return error_count; }
std::uint32_t duplicate_frames() { return duplicate_count; }
std::uint64_t last_contact_ticks() { return last_contact; }

extern "C" void USART1_IRQHandler() { HAL_UART_IRQHandler(&uart); }

extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef *handle) {
  if (handle != &uart) {
    return;
  }
  const std::uint16_t next =
      static_cast<std::uint16_t>((receive_write + 1U) % receive_ring.size());
  if (next != receive_read) {
    receive_ring[receive_write] = receive_byte;
    __DMB();
    receive_write = next;
  } else {
    ++error_count;
  }
  (void)HAL_UART_Receive_IT(&uart, &receive_byte, 1);
}

extern "C" void HAL_UART_ErrorCallback(UART_HandleTypeDef *handle) {
  if (handle == &uart) {
    ++error_count;
    (void)HAL_UART_AbortReceive(handle);
    (void)HAL_UART_Receive_IT(&uart, &receive_byte, 1);
  }
}

} // namespace node_rs485
