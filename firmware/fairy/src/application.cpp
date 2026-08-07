#include "application.hpp"

#include <array>
#include <cstring>

#include "board_profile.hpp"
#include "debug_log.hpp"
#include "fairy_shared/adelie_protocol.hpp"
#include "fairy_shared/fairy_protocol.hpp"
#include "fairy_shared/tlv.hpp"
#include "light_sensor.hpp"
#include "outputs.hpp"
#include "record_store.hpp"
#include "rs485_link.hpp"
#include "timebase.hpp"

namespace fairy_application {
namespace {

using fairy::protocol::AdelieMessageView;
using fairy::protocol::CommandField;
using fairy::protocol::Field;
using fairy::protocol::MessageHeader;
using fairy::protocol::MessageKind;
using fairy::protocol::Opcode;
using fairy::protocol::RecordType;
using fairy::protocol::Status;
using fairy::protocol::TlvReader;
using fairy::protocol::TlvWriter;

struct ScheduledCommand {
  std::array<std::uint8_t, fairy::protocol::adelie_max_message_size> message{};
  std::uint16_t length{};
  std::uint64_t execute_at{};
};

std::array<ScheduledCommand, 8> scheduled;
std::size_t scheduled_count;
std::uint32_t active_session;
std::uint32_t last_command_id;
std::uint64_t last_health_ticks;
bool boot_light_passed;
std::uint64_t last_light_ticks;
inline constexpr std::uint64_t light_gate_rearm_ticks = 1'600'000ULL;

unsigned long ticks_high(std::uint64_t ticks) {
  return static_cast<unsigned long>(ticks >> 32U);
}

unsigned long ticks_low(std::uint64_t ticks) {
  return static_cast<unsigned long>(ticks & 0xFFFFFFFFULL);
}

void clear_scheduled() { scheduled_count = 0; }

bool add_scheduled(const ScheduledCommand &command) {
  if (scheduled_count == scheduled.size()) {
    return false;
  }
  std::size_t position = scheduled_count;
  while (position > 0U &&
         scheduled[position - 1U].execute_at > command.execute_at) {
    scheduled[position] = scheduled[position - 1U];
    --position;
  }
  scheduled[position] = command;
  ++scheduled_count;
  return true;
}

ScheduledCommand take_first_scheduled() {
  ScheduledCommand first = scheduled[0];
  for (std::size_t index = 1; index < scheduled_count; ++index) {
    scheduled[index - 1U] = scheduled[index];
  }
  --scheduled_count;
  return first;
}

bool field_u8(const AdelieMessageView &message, CommandField wanted,
              std::uint8_t &value) {
  TlvReader reader(message.payload, message.header.payload_length);
  fairy::protocol::FieldView field;
  while (reader.next(field)) {
    if (field.tag == static_cast<std::uint16_t>(wanted)) {
      return TlvReader::as_u8(field, value);
    }
  }
  return false;
}

bool field_u16(const AdelieMessageView &message, CommandField wanted,
               std::uint16_t &value) {
  TlvReader reader(message.payload, message.header.payload_length);
  fairy::protocol::FieldView field;
  while (reader.next(field)) {
    if (field.tag == static_cast<std::uint16_t>(wanted)) {
      return TlvReader::as_u16(field, value);
    }
  }
  return false;
}

bool field_u32(const AdelieMessageView &message, CommandField wanted,
               std::uint32_t &value) {
  TlvReader reader(message.payload, message.header.payload_length);
  fairy::protocol::FieldView field;
  while (reader.next(field)) {
    if (field.tag == static_cast<std::uint16_t>(wanted)) {
      return TlvReader::as_u32(field, value);
    }
  }
  return false;
}

void command_result(const AdelieMessageView &command, Status status,
                    std::uint64_t actual_ticks) {
  std::uint8_t payload[80]{};
  TlvWriter fields(payload, sizeof(payload));
  (void)fields.u32(static_cast<std::uint16_t>(Field::command_id),
                   command.header.command_id);
  (void)fields.u16(static_cast<std::uint16_t>(Field::operation),
                   static_cast<std::uint16_t>(command.header.opcode));
  (void)fields.u8(static_cast<std::uint16_t>(Field::status),
                  static_cast<std::uint8_t>(status));
  (void)fields.u64(static_cast<std::uint16_t>(Field::requested_ticks),
                   command.header.execute_at_ticks);
  (void)fields.u64(static_cast<std::uint16_t>(Field::actual_ticks),
                   actual_ticks);
  std::uint16_t flags =
      fairy::protocol::critical | fairy::protocol::actual_time;
  if (command.header.execute_at_ticks != 0U) {
    flags |= fairy::protocol::scheduled;
  }
  (void)fairy_records::enqueue(RecordType::command_result, flags, actual_ticks,
                               active_session, payload, fields.size());
}

Status execute(const AdelieMessageView &command, std::uint64_t now_ticks) {
  if (command.header.command_id == last_command_id &&
      command.header.command_id != 0U) {
    return Status::duplicate;
  }

  const bool session_command = command.header.opcode == Opcode::start_session ||
                               command.header.opcode == Opcode::stop_session;
  const bool manual_output_command =
      command.header.opcode == Opcode::set_rgb ||
      command.header.opcode == Opcode::set_ir ||
      command.header.opcode == Opcode::set_audio ||
      command.header.opcode == Opcode::actuate_valve;
  const bool allowed_without_session =
      session_command || manual_output_command ||
      command.header.opcode == Opcode::ping ||
      command.header.opcode == Opcode::identify ||
      command.header.opcode == Opcode::get_health ||
      command.header.opcode == Opcode::clear_faults ||
      command.header.opcode == Opcode::configure_valve;

  if (!allowed_without_session &&
      (active_session == 0U || command.header.session_id != active_session)) {
    return Status::session_mismatch;
  }

  Status status = Status::ok;
  switch (command.header.opcode) {
  case Opcode::ping:
  case Opcode::get_health:
  case Opcode::clear_faults:
    break;

  case Opcode::start_session:
    if (command.header.session_id == 0U) {
      status = Status::invalid_parameter;
    } else if (active_session != 0U &&
               active_session != command.header.session_id) {
      status = Status::busy;
    } else {
      fairy_outputs::all_safe(now_ticks);
      active_session = command.header.session_id;
      if (boot_light_passed) {
        fairy_outputs::set_ir(true, now_ticks);
      }
    }
    break;

  case Opcode::stop_session:
    if (active_session != 0U && command.header.session_id != active_session) {
      status = Status::session_mismatch;
    } else {
      fairy_outputs::all_safe(now_ticks);
      active_session = 0U;
      clear_scheduled();
    }
    break;

  case Opcode::identify: {
    std::uint32_t duration_ms = 3000U;
    (void)field_u32(command, CommandField::identify_duration_ms, duration_ms);
    duration_ms = duration_ms > 10'000U ? 10'000U : duration_ms;
    fairy_outputs::set_rgb(255, 255, 255, duration_ms, now_ticks);
    break;
  }

  case Opcode::set_rgb: {
    std::uint8_t red{};
    std::uint8_t green{};
    std::uint8_t blue{};
    std::uint32_t duration{};
    if (!field_u8(command, CommandField::red, red) ||
        !field_u8(command, CommandField::green, green) ||
        !field_u8(command, CommandField::blue, blue)) {
      status = Status::invalid_parameter;
      break;
    }
    (void)field_u32(command, CommandField::duration_ms, duration);
    fairy_outputs::set_rgb(red, green, blue, duration, now_ticks);
    break;
  }

  case Opcode::set_ir: {
    std::uint8_t enabled{};
    if (!field_u8(command, CommandField::enabled, enabled) || enabled > 1U) {
      status = Status::invalid_parameter;
    } else {
      fairy_outputs::set_ir(enabled != 0U, now_ticks);
    }
    break;
  }

  case Opcode::set_audio: {
    std::uint8_t mode{};
    std::uint16_t amplitude{};
    std::uint32_t frequency{};
    std::uint32_t low{};
    std::uint32_t high{};
    std::uint32_t duration{};
    if (!field_u8(command, CommandField::audio_mode, mode) ||
        !field_u16(command, CommandField::amplitude, amplitude)) {
      status = Status::invalid_parameter;
      break;
    }
    (void)field_u32(command, CommandField::frequency_hz, frequency);
    (void)field_u32(command, CommandField::low_frequency_hz, low);
    (void)field_u32(command, CommandField::high_frequency_hz, high);
    (void)field_u32(command, CommandField::duration_ms, duration);
    status = fairy_outputs::set_audio(
        static_cast<fairy_outputs::AudioMode>(mode), frequency, low, high,
        amplitude, duration, now_ticks);
    break;
  }

  case Opcode::configure_valve: {
    fairy_outputs::ValveConfiguration configuration;
    if (!field_u32(command, CommandField::vload_millivolts,
                   configuration.vload_mv) ||
        !field_u32(command, CommandField::spike_duration_us,
                   configuration.spike_duration_us) ||
        !field_u16(command, CommandField::spike_duty_per_mille,
                   configuration.spike_duty_per_mille) ||
        !field_u16(command, CommandField::hold_duty_per_mille,
                   configuration.hold_duty_per_mille) ||
        !field_u32(command, CommandField::max_on_time_us,
                   configuration.maximum_on_us) ||
        !field_u32(command, CommandField::minimum_interval_us,
                   configuration.minimum_interval_us)) {
      status = Status::invalid_parameter;
      break;
    }
    status = fairy_outputs::configure_valve(configuration);
    break;
  }

  case Opcode::actuate_valve: {
    std::uint32_t duration{};
    if ((command.header.flags & fairy::protocol::safety_authorized) == 0U ||
        !field_u32(command, CommandField::duration_ms, duration)) {
      status = Status::safety_lock;
      fairy_debug::log("VALVE rejected status=%u\r\n",
                       static_cast<unsigned int>(status));
      break;
    }
    status = fairy_outputs::actuate_valve(duration, now_ticks);
    fairy_debug::log("VALVE dwell_ms=%lu transition_us=8000 status=%u\r\n",
                     static_cast<unsigned long>(duration),
                     static_cast<unsigned int>(status));
    break;
  }

  default:
    status = Status::unsupported;
    break;
  }

  last_command_id = command.header.command_id;
  command_result(command, status, now_ticks);
  return status;
}

std::size_t response_for(const AdelieMessageView &command, Status status,
                         std::uint8_t *response, std::size_t capacity) {
  MessageHeader header = command.header;
  header.kind = MessageKind::response;
  header.status = status;
  header.flags = 0;
  header.payload_length = 0;
  return fairy::protocol::encode_adelie(header, nullptr, 0, response, capacity);
}

void output_events() {
  fairy_outputs::Event event;
  while (fairy_outputs::pop_event(event)) {
    std::uint8_t payload[64]{};
    TlvWriter fields(payload, sizeof(payload));
    (void)fields.u16(static_cast<std::uint16_t>(Field::operation),
                     static_cast<std::uint16_t>(event.kind));
    (void)fields.u32(static_cast<std::uint16_t>(Field::value), event.value);
    (void)fields.u32(static_cast<std::uint16_t>(Field::duration_us),
                     event.duration_us);
    (void)fairy_records::enqueue(
        RecordType::output_change,
        fairy::protocol::critical | fairy::protocol::actual_time, event.ticks,
        active_session, payload, fields.size());
  }
}

void light_gate_events() {
  fairy_light::GateEvent gate_event;
  while (fairy_light::pop_gate_event(gate_event)) {
    if (last_light_ticks != 0U &&
        gate_event.ticks - last_light_ticks < light_gate_rearm_ticks) {
      fairy_debug::log(
          "LIGHT_GATE_EVENT status=debounced ticks_hi=%lu ticks_lo=%lu "
          "delta_us=%lu adc=%u source=%s\r\n",
          ticks_high(gate_event.ticks), ticks_low(gate_event.ticks),
          static_cast<unsigned long>((gate_event.ticks - last_light_ticks) /
                                     16U),
          static_cast<unsigned int>(gate_event.adc_value),
          gate_event.hardware_capture ? "tim2" : "adc");
      continue;
    }

    last_light_ticks = gate_event.ticks;
    std::uint8_t payload[48]{};
    TlvWriter fields(payload, sizeof(payload));
    (void)fields.u8(static_cast<std::uint16_t>(Field::state), 1U);
    (void)fields.u16(static_cast<std::uint16_t>(Field::adc_value),
                     gate_event.adc_value);
    (void)fields.boolean(static_cast<std::uint16_t>(Field::detail),
                         gate_event.overcapture);
    std::uint16_t flags = fairy::protocol::critical;
    if (gate_event.overcapture) {
      flags |= fairy::protocol::loss_latched;
    }
    const bool queued =
        fairy_records::enqueue(RecordType::light_gate, flags, gate_event.ticks,
                               active_session, payload, fields.size());
    fairy_debug::log(
        "LIGHT_GATE_EVENT status=%s session=%lu adc=%u ticks_hi=%lu "
        "ticks_lo=%lu source=%s overcapture=%u queue_depth=%u dropped=%lu\r\n",
        queued ? "queued" : "queue_full",
        static_cast<unsigned long>(active_session),
        static_cast<unsigned int>(gate_event.adc_value),
        ticks_high(gate_event.ticks), ticks_low(gate_event.ticks),
        gate_event.hardware_capture ? "tim2" : "adc",
        gate_event.overcapture ? 1U : 0U,
        static_cast<unsigned int>(fairy_records::size()),
        static_cast<unsigned long>(fairy_records::dropped()));
  }
}

void health(std::uint64_t now_ticks) {
  if (now_ticks - last_health_ticks < 16'000'000ULL) {
    return;
  }
  last_health_ticks = now_ticks;
  std::uint8_t payload[96]{};
  TlvWriter fields(payload, sizeof(payload));
  (void)fields.u32(static_cast<std::uint16_t>(Field::uptime_ms),
                   static_cast<std::uint32_t>(now_ticks / 16'000ULL));
  (void)fields.u8(static_cast<std::uint16_t>(Field::queue_depth),
                  static_cast<std::uint8_t>(fairy_records::size()));
  (void)fields.u8(static_cast<std::uint16_t>(Field::queue_capacity), 48U);
  (void)fields.u32(static_cast<std::uint16_t>(Field::dropped_records),
                   fairy_records::dropped());
  (void)fields.u32(static_cast<std::uint16_t>(Field::transport_errors),
                   fairy_rs485::transport_errors());
  (void)fields.u32(static_cast<std::uint16_t>(Field::duplicate_frames),
                   fairy_rs485::duplicate_frames());
  (void)fields.u16(static_cast<std::uint16_t>(Field::adc_value),
                   fairy_light::last_value());
  (void)fields.boolean(static_cast<std::uint16_t>(Field::state),
                       boot_light_passed);
  (void)fairy_records::enqueue(RecordType::health, 0, now_ticks, active_session,
                               payload, fields.size());
}

} // namespace

void initialize(std::uint32_t reset_cause, bool light_test_passed,
                std::uint16_t dark_adc, std::uint16_t clear_adc) {
  active_session = 0;
  last_command_id = 0;
  last_health_ticks = 0;
  clear_scheduled();
  boot_light_passed = light_test_passed;
  last_light_ticks = 0;

  std::uint8_t payload[112]{};
  TlvWriter fields(payload, sizeof(payload));
  (void)fields.string(static_cast<std::uint16_t>(Field::board_kind),
                      fairy_board::name);
  (void)fields.string(static_cast<std::uint16_t>(Field::firmware_version),
                      "3.0.1");
  (void)fields.bytes(static_cast<std::uint16_t>(Field::uuid),
                     fairy_rs485::uuid().data(),
                     static_cast<std::uint8_t>(fairy_rs485::uuid().size()));
  (void)fields.u32(static_cast<std::uint16_t>(Field::reset_cause), reset_cause);
  (void)fields.u16(static_cast<std::uint16_t>(Field::adc_value), clear_adc);
  (void)fields.boolean(static_cast<std::uint16_t>(Field::state),
                       light_test_passed);
  (void)fairy_records::enqueue(
      RecordType::boot, fairy::protocol::first_after_reset,
      fairy_timebase::now(), 0, payload, fields.size());

  if (!light_test_passed) {
    std::uint8_t fault_payload[64]{};
    TlvWriter fault(fault_payload, sizeof(fault_payload));
    (void)fault.string(static_cast<std::uint16_t>(Field::reason),
                       "light gate self test");
    (void)fault.u32(static_cast<std::uint16_t>(Field::value), dark_adc);
    (void)fault.u16(static_cast<std::uint16_t>(Field::adc_value), clear_adc);
    (void)fairy_records::enqueue(RecordType::fault, fairy::protocol::critical,
                                 fairy_timebase::now(), 0, fault_payload,
                                 fault.size());
  }
}

std::size_t handle_adelie(const std::uint8_t *message, std::size_t length,
                          std::uint8_t *response, std::size_t capacity) {
  AdelieMessageView command;
  if (!fairy::protocol::decode_adelie(message, length, command) ||
      command.header.kind != MessageKind::command) {
    return 0;
  }

  const std::uint64_t now_ticks = fairy_timebase::now();
  if (command.header.execute_at_ticks != 0U &&
      command.header.execute_at_ticks > now_ticks) {
    ScheduledCommand stored;
    if (length > stored.message.size()) {
      return response_for(command, Status::bad_message, response, capacity);
    }
    std::memcpy(stored.message.data(), message, length);
    stored.length = static_cast<std::uint16_t>(length);
    stored.execute_at = command.header.execute_at_ticks;
    const Status status =
        add_scheduled(stored) ? Status::accepted : Status::queue_full;
    return response_for(command, status, response, capacity);
  }

  const Status status = execute(command, now_ticks);
  return response_for(command, status, response, capacity);
}

void service() {
  const std::uint64_t now_ticks = fairy_timebase::now();
  fairy_outputs::service(now_ticks);

  if (active_session != 0U &&
      now_ticks - fairy_rs485::last_contact_ticks() > 32'000'000ULL) {
    fairy_outputs::all_safe(now_ticks);
    active_session = 0;
    clear_scheduled();
    std::uint8_t payload[48]{};
    TlvWriter fields(payload, sizeof(payload));
    (void)fields.string(static_cast<std::uint16_t>(Field::reason),
                        "communication timeout");
    (void)fairy_records::enqueue(RecordType::fault,
                                 fairy::protocol::critical |
                                     fairy::protocol::loss_latched,
                                 now_ticks, 0, payload, fields.size());
  }

  while (scheduled_count != 0U) {
    if (scheduled[0].execute_at > now_ticks) {
      break;
    }
    const ScheduledCommand command = take_first_scheduled();
    AdelieMessageView view;
    if (fairy::protocol::decode_adelie(command.message.data(), command.length,
                                       view)) {
      (void)execute(view, now_ticks);
    }
  }

  fairy_timebase::Capture capture;
  while (fairy_timebase::pop_capture(capture)) {
    if (capture.kind == fairy_timebase::CaptureKind::light_gate) {
      fairy_light::capture_edge(capture.ticks, capture.overcapture);
      continue;
    }
    std::uint8_t payload[48]{};
    TlvWriter fields(payload, sizeof(payload));
    (void)fields.u8(static_cast<std::uint16_t>(Field::state), 1U);
    (void)fields.boolean(static_cast<std::uint16_t>(Field::detail),
                         capture.overcapture);
    constexpr auto type = RecordType::sync_observation;
    std::uint16_t flags = 0U;
    if (capture.overcapture) {
      flags |= fairy::protocol::loss_latched;
    }
    (void)fairy_records::enqueue(type, flags, capture.ticks, active_session,
                                 payload, fields.size());
  }

  /* Prefer a queued TIM2 edge; use the ADC transition only when PA1 did not
   * reach a digital-high level for a partial beam interruption. */
  fairy_light::service();
  light_gate_events();

  output_events();
  health(now_ticks);
}

std::uint32_t session_id() { return active_session; }
bool session_active() { return active_session != 0U; }

} // namespace fairy_application
