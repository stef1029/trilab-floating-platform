#pragma once

#include <cstdint>

namespace fairy_timebase {

enum class CaptureKind : std::uint8_t {
  sync,
  light_gate,
};

struct Capture {
  CaptureKind kind{};
  std::uint64_t ticks{};
  bool overcapture{};
};

void initialize();
std::uint64_t now();
bool pop_capture(Capture &capture);
void busy_wait_us(std::uint32_t microseconds);

} // namespace fairy_timebase
