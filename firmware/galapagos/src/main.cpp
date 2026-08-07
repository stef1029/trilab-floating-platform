#include <zephyr/kernel.h>

#include "application.hpp"
#include "ble_service.hpp"
#include "debug_log.hpp"
#include "hardware.hpp"
#include "record_stream.hpp"

int main() {
  galapagos_stream::initialize();
  int error = galapagos_hardware::initialize();
  if (error == 0) {
    galapagos_application::initialize();
    error = galapagos_ble::initialize();
  }
  if (error != 0) {
    galapagos_debug::log("FATAL galapagos status=%d\r\n", error);
  } else {
    galapagos_debug::log(
        "READY galapagos fairy=3 adelie=2 transport=1 timer_hz=16000000\r\n");
  }
  while (true) {
    k_sleep(K_SECONDS(60));
  }
  return 0;
}
