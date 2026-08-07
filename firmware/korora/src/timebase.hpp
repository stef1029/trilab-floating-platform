#pragma once

#include <cstdint>

namespace korora_time {

enum class CaptureKind : std::uint8_t {
  external_event,
  ttl_input,
};

struct Capture {
  CaptureKind kind{};
  std::uint64_t ticks{};
  std::uint32_t sequence{};
};

int initialize();
std::uint64_t now();
std::uint32_t pulse_count();
std::uint64_t pulse_ticks(std::uint32_t pulse);
bool pop_capture(Capture &capture);
std::uint32_t ttl_capture_count();
std::uint32_t ttl_capture_drops();

} // namespace korora_time
