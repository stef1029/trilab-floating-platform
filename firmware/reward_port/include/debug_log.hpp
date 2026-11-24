#pragma once

#include <cstdarg>

namespace node_debug {

void initialize();
void log(const char *format, ...);
void service();

} // namespace node_debug
