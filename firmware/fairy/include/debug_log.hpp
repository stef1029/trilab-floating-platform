#pragma once

#include <cstdarg>

namespace fairy_debug {

void initialize();
void log(const char *format, ...);
void service();

} // namespace fairy_debug
