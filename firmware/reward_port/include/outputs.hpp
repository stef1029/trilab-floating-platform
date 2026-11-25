#pragma once

#include <cstdint>

#include "rig_protocol/command_protocol.hpp"

namespace node_outputs {

enum class AudioMode : std::uint8_t {
  off = 0,
  tone = 1,
  white_noise_band = 2,
};

enum class EventKind : std::uint8_t {
  rgb,
  audio_started,
  audio_stopped,
  valve_started,
  valve_stopped,
  ir,
};

struct Event {
  EventKind kind{};
  std::uint64_t ticks{};
  std::uint32_t value{};
  std::uint32_t duration_us{};
};

struct ValveConfiguration {
  std::uint32_t vload_mv{5000};
  /*
   * Spike and hold fields are retained for Command protocol v2 compatibility
   * and are ignored: the non-latching valve is driven at full duty for the
   * whole open time. maximum_on_us is therefore the maximum coil on time.
   */
  std::uint32_t spike_duration_us{8000};
  std::uint16_t spike_duty_per_mille{1000};
  std::uint16_t hold_duty_per_mille{0};
  std::uint32_t maximum_on_us{250000};
  std::uint32_t minimum_interval_us{250000};
};

void initialize();
void service(std::uint64_t now_ticks);
void all_safe(std::uint64_t now_ticks = 0);

void set_rgb(std::uint8_t red, std::uint8_t green, std::uint8_t blue,
             std::uint32_t duration_ms, std::uint64_t now_ticks);
void set_ir(bool enabled, std::uint64_t now_ticks);
/* Sets the IR resting state that all_safe() restores, and applies it now. */
void set_ir_default(bool enabled, std::uint64_t now_ticks);

rig::protocol::Status set_audio(AudioMode mode, std::uint32_t frequency_hz,
                                  std::uint32_t low_hz, std::uint32_t high_hz,
                                  std::uint16_t amplitude,
                                  std::uint32_t duration_ms,
                                  std::uint64_t now_ticks);

rig::protocol::Status
configure_valve(const ValveConfiguration &configuration);
rig::protocol::Status actuate_valve(std::uint32_t duration_ms,
                                      std::uint64_t now_ticks);

bool pop_event(Event &event);
const ValveConfiguration &valve_configuration();

} // namespace node_outputs
