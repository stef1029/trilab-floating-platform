#pragma once

#include <cstdint>

namespace fairy_light {

struct SelfTest {
  std::uint16_t dark{};
  std::uint16_t clear{};
  bool passed{};
  std::uint16_t clear_threshold{};
  std::uint16_t blocked_threshold{};
  bool fixed_thresholds{};
};

struct GateEvent {
  std::uint64_t ticks{};
  std::uint16_t adc_value{};
  bool overcapture{};
  bool hardware_capture{};
};

bool initialize();
std::uint16_t read();
SelfTest self_test();
void set_emitter_enabled(bool enabled);
void service();
void capture_edge(std::uint64_t ticks, bool overcapture);
bool pop_gate_event(GateEvent &event);
std::uint16_t last_value();
bool monitoring();

} // namespace fairy_light
