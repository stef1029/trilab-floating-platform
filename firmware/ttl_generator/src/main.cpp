#include <zephyr/kernel.h>

#include "application.hpp"
#include "ble_service.hpp"
#include "debug_log.hpp"
#include "hardware.hpp"
#include "record_stream.hpp"

int main() {
  ttl_generator_stream::initialize();
  int error = ttl_generator_hardware::initialize();
  if (error == 0) {
    ttl_generator_application::initialize();
    error = ttl_generator_ble::initialize();
  }
  if (error != 0) {
    ttl_generator_debug::log("FATAL ttl_generator status=%d\r\n", error);
  } else {
    ttl_generator_debug::log(
        "READY ttl_generator node=3 host=2 transport=1 timer_hz=16000000\r\n");
  }
  while (true) {
    k_sleep(K_SECONDS(60));
  }
  return 0;
}
