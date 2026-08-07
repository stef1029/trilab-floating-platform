#include "rs485_link.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include <stm32g0xx_hal.h>

#include "application.hpp"
#include "board_profile.hpp"
#include "debug_log.hpp"
#include "fairy_shared/bytes.hpp"
#include "fairy_shared/fairy_protocol.hpp"
#include "fairy_shared/magellan_protocol.hpp"
#include "fairy_shared/system_config.hpp"
#include "fairy_shared/transport.hpp"
#include "outputs.hpp"
#include "record_store.hpp"
#include "timebase.hpp"

#ifndef UID_BASE
#define UID_BASE 0x1FFF7590UL
#endif

namespace fairy_rs485 {
namespace {

UART_HandleTypeDef uart;
std::uint8_t receive_byte;
std::array<std::uint8_t, 512> receive_ring;
volatile std::uint16_t receive_read;
volatile std::uint16_t receive_write;

fairy::transport::UartDecoder decoder;
fairy::transport::Reassembler reassembler;
fairy::protocol::DeviceUuid device_uuid;
std::uint8_t local_address;
std::uint8_t local_logical_slot = 0xFF;
volatile std::uint32_t error_count;
std::uint32_t duplicate_count;
std::uint16_t last_request_transfer;
fairy::transport::Channel last_request_channel =
    fairy::transport::Channel::fairy;
bool last_request_valid;
std::uint16_t outstanding_record_transfer;
bool outstanding_record;
std::uint64_t last_contact;

struct PendingOffer {
  bool active{};
  std::uint64_t send_ticks{};
  std::uint32_t nonce{};
  std::uint16_t transfer_id{};
};

PendingOffer pending_offer;

void uart_initialize() {
  __HAL_RCC_USART1_CLK_ENABLE();
  uart.Instance = USART1;
  uart.Init.BaudRate = fairy::config::rs485_baud;
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
  std::uint8_t encoded[fairy::transport::max_encoded_uart_frame]{};
  const std::size_t encoded_length =
      fairy::transport::cobs_encode(raw, raw_length, encoded, sizeof(encoded));
  if (encoded_length == 0U) {
    ++error_count;
    return false;
  }

  fairy_timebase::busy_wait_us(40);
  fairy_board::set_rs485_transmit(true);
  fairy_timebase::busy_wait_us(1);
  const HAL_StatusTypeDef status = HAL_UART_Transmit(
      &uart, encoded, static_cast<std::uint16_t>(encoded_length), 20);
  fairy_board::set_rs485_transmit(false);
  if (status != HAL_OK) {
    ++error_count;
    return false;
  }
  return true;
}

bool send_message(fairy::transport::Channel channel, std::uint8_t flags,
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
    fairy::transport::Header header;
    header.channel = channel;
    header.flags = flags;
    if (index == 0U) {
      header.flags |= fairy::transport::first_fragment;
    }
    if (index + 1U == fragment_count) {
      header.flags |= fairy::transport::last_fragment;
    }
    header.source = source;
    header.destination = destination;
    header.fragment_index = static_cast<std::uint8_t>(index);
    header.fragment_count = static_cast<std::uint8_t>(fragment_count);
    header.transfer_id = transfer_id;

    std::uint8_t raw[fairy::transport::max_raw_frame_size]{};
    const std::size_t raw_length = fairy::transport::encode_frame(
        header, length == 0U ? nullptr : message + offset, chunk, raw,
        sizeof(raw));
    if (raw_length == 0U || !transmit_raw(raw, raw_length)) {
      return false;
    }
  }
  return true;
}

void send_poll_response(std::uint16_t transfer_id) {
  const fairy_records::StoredRecord *record = fairy_records::front();
  if (record == nullptr) {
    (void)send_message(fairy::transport::Channel::fairy,
                       fairy::transport::response, local_address,
                       fairy::config::korora_address, transfer_id, nullptr, 0);
    return;
  }
  if (send_message(fairy::transport::Channel::fairy, fairy::transport::response,
                   local_address, fairy::config::korora_address, transfer_id,
                   record->bytes.data(), record->length)) {
    outstanding_record = true;
    outstanding_record_transfer = transfer_id;
  }
}

void send_offer() {
  fairy::protocol::Offer offer;
  offer.uuid = device_uuid;
  offer.capabilities =
      fairy::protocol::capability_light_gate | fairy::protocol::capability_rgb |
      fairy::protocol::capability_audio | fairy::protocol::capability_valve |
      fairy::protocol::capability_ir | fairy::protocol::capability_sync_capture;
  offer.boot_count = 0;
  std::uint8_t offer_payload[32]{};
  const std::size_t offer_payload_length = fairy::protocol::encode_offer(
      offer, offer_payload, sizeof(offer_payload));
  fairy::protocol::MagellanHeader header;
  header.type = fairy::protocol::MagellanType::offer;
  header.nonce = pending_offer.nonce;
  std::uint8_t message[64]{};
  const std::size_t message_length = fairy::protocol::encode_magellan(
      header, offer_payload, offer_payload_length, message, sizeof(message));
  (void)send_message(fairy::transport::Channel::magellan, 0, local_address,
                     fairy::config::korora_address, pending_offer.transfer_id,
                     message, message_length);
  pending_offer.active = false;
}

void handle_magellan(const fairy::transport::MessageView &message) {
  fairy::protocol::MagellanView view;
  if (!fairy::protocol::decode_magellan(message.payload, message.payload_length,
                                        view)) {
    ++error_count;
    return;
  }

  if (view.header.type == fairy::protocol::MagellanType::discover &&
      local_address == fairy::config::unassigned_address) {
    if (view.header.payload_length != 4U) {
      return;
    }
    const std::uint8_t slot_count = view.payload[0];
    const std::uint16_t slot_us = fairy::wire::get_u16(view.payload + 1);
    if (slot_count == 0U || slot_count > 32U || slot_us < 500U) {
      return;
    }
    const std::uint8_t slot = fairy::protocol::discovery_slot(
        device_uuid, view.header.nonce, slot_count);
    pending_offer.active = true;
    pending_offer.nonce = view.header.nonce;
    pending_offer.transfer_id = message.transfer_id;
    pending_offer.send_ticks =
        fairy_timebase::now() +
        static_cast<std::uint64_t>(200U + slot * slot_us) * 16U;
    return;
  }

  if (view.header.type == fairy::protocol::MagellanType::assign) {
    fairy::protocol::Assignment assignment;
    if (!fairy::protocol::decode_assignment(
            view.payload, view.header.payload_length, assignment) ||
        assignment.uuid != device_uuid) {
      return;
    }
    local_address = assignment.address;
    local_logical_slot = assignment.logical_slot;

    fairy::protocol::MagellanHeader response_header;
    response_header.type = fairy::protocol::MagellanType::assigned;
    response_header.nonce = view.header.nonce;
    std::uint8_t assignment_payload[16]{};
    const std::size_t assignment_length = fairy::protocol::encode_assignment(
        assignment, assignment_payload, sizeof(assignment_payload));
    std::uint8_t response[48]{};
    const std::size_t response_length = fairy::protocol::encode_magellan(
        response_header, assignment_payload, assignment_length, response,
        sizeof(response));
    (void)send_message(fairy::transport::Channel::magellan,
                       fairy::transport::response, local_address,
                       fairy::config::korora_address, message.transfer_id,
                       response, response_length);
    return;
  }

  if (view.header.type == fairy::protocol::MagellanType::release) {
    if (view.header.payload_length == 0U) {
      local_address = fairy::config::unassigned_address;
      local_logical_slot = 0xFF;
      outstanding_record = false;
      pending_offer.active = false;
      fairy_outputs::all_safe(fairy_timebase::now());
      return;
    }
    fairy::protocol::Assignment assignment;
    if (fairy::protocol::decode_assignment(
            view.payload, view.header.payload_length, assignment) &&
        assignment.uuid == device_uuid) {
      local_address = fairy::config::unassigned_address;
      local_logical_slot = 0xFF;
      fairy_outputs::all_safe(fairy_timebase::now());
    }
  }
}

void handle_message(const fairy::transport::MessageView &message) {
  const bool addressed =
      message.destination == local_address ||
      message.destination == fairy::config::broadcast_address ||
      (local_address == fairy::config::unassigned_address &&
       message.destination == fairy::config::unassigned_address);
  if (!addressed || message.source != fairy::config::korora_address) {
    return;
  }
  last_contact = fairy_timebase::now();

  const bool is_acknowledgement =
      (message.flags & fairy::transport::acknowledgement) != 0U;
  if (!is_acknowledgement) {
    if (last_request_valid && message.transfer_id == last_request_transfer &&
        message.channel == last_request_channel) {
      ++duplicate_count;
    }
    last_request_valid = true;
    last_request_transfer = message.transfer_id;
    last_request_channel = message.channel;
  }

  if (message.channel == fairy::transport::Channel::magellan) {
    handle_magellan(message);
    return;
  }

  if (message.channel == fairy::transport::Channel::fairy) {
    if ((message.flags & fairy::transport::acknowledgement) != 0U) {
      if (outstanding_record &&
          outstanding_record_transfer == message.transfer_id) {
        (void)fairy_records::pop();
        outstanding_record = false;
      }
      return;
    }
    if (message.payload_length == 0U) {
      send_poll_response(message.transfer_id);
    }
    return;
  }

  if (message.channel == fairy::transport::Channel::adelie) {
    std::uint8_t response[fairy::protocol::adelie_max_message_size]{};
    const std::size_t response_length = fairy_application::handle_adelie(
        message.payload, message.payload_length, response, sizeof(response));
    if (response_length != 0U) {
      (void)send_message(fairy::transport::Channel::adelie,
                         fairy::transport::response, local_address,
                         fairy::config::korora_address, message.transfer_id,
                         response, response_length);
    }
  }
}

void consume_received() {
  while (receive_read != receive_write) {
    const std::uint8_t byte = receive_ring[receive_read];
    receive_read =
        static_cast<std::uint16_t>((receive_read + 1U) % receive_ring.size());
    fairy::transport::FrameView frame;
    const auto result = decoder.push(byte, frame);
    if (result == fairy::transport::DecodeResult::ok) {
      fairy::transport::MessageView message;
      const auto reassembly = reassembler.accept(frame, message);
      if (reassembly == fairy::transport::DecodeResult::ok) {
        handle_message(message);
      } else if (reassembly != fairy::transport::DecodeResult::incomplete) {
        ++error_count;
      }
    } else if (result != fairy::transport::DecodeResult::incomplete) {
      ++error_count;
    }
  }
}

} // namespace

void initialize() {
  local_address = fairy::config::unassigned_address;
  local_logical_slot = 0xFF;
  last_contact = fairy_timebase::now();
  const volatile std::uint32_t *uid =
      reinterpret_cast<const volatile std::uint32_t *>(UID_BASE);
  for (std::size_t word = 0; word < 3; ++word) {
    const std::uint32_t value = uid[word];
    fairy::wire::put_u32(device_uuid.data() + word * 4U, value);
  }
  uart_initialize();
}

void service() {
  consume_received();
  if (pending_offer.active &&
      fairy_timebase::now() >= pending_offer.send_ticks) {
    send_offer();
  }
}

const fairy::protocol::DeviceUuid &uuid() { return device_uuid; }
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

} // namespace fairy_rs485
