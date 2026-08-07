#include "debug_log.hpp"

#include <cstdarg>
#include <cstdio>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "fairy_shared/system_config.hpp"

namespace galapagos_debug {
namespace {

struct Line {
  char text[192];
};

K_MSGQ_DEFINE(lines, sizeof(Line), 16, 4);

void thread(void *, void *, void *) {
  Line line;
  while (true) {
    k_msgq_get(&lines, &line, K_FOREVER);
#if FAIRY_ENABLE_DEBUG_STREAM
    printk("%s", line.text);
#endif
  }
}

K_THREAD_DEFINE(debug_thread_id, 1536, thread, nullptr, nullptr, nullptr, 8, 0,
                0);

} // namespace

void log(const char *format, ...) {
#if FAIRY_ENABLE_DEBUG_STREAM
  Line line{};
  va_list arguments;
  va_start(arguments, format);
  const int length = vsnprintk(line.text, sizeof(line.text), format, arguments);
  va_end(arguments);
  if (length > 0 && length < static_cast<int>(sizeof(line.text))) {
    (void)k_msgq_put(&lines, &line, K_NO_WAIT);
  }
#else
  (void)format;
#endif
}

} // namespace galapagos_debug
