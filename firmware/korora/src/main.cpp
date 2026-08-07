#include <zephyr/kernel.h>

#include "ble_gateway.hpp"
#include "control.hpp"
#include "debug_log.hpp"
#include "experiment.hpp"
#include "fairy_manager.hpp"
#include "galapagos_manager.hpp"
#include "rs485_bus.hpp"
#include "timebase.hpp"

namespace {

bool start_component(const char *name, int result) {
  korora_debug::log("START component=%s status=%d\r\n", name, result);
  return result == 0;
}

} // namespace

int main() {
  if (!start_component("timebase", korora_time::initialize()) ||
      !start_component("rs485", korora_rs485::initialize())) {
    return 1;
  }

  korora_galapagos::initialize();
  korora_control::initialize();
  korora_experiment::initialize();

  if (!start_component("fairies", korora_fairies::initialize()) ||
      !start_component("ble", korora_ble::initialize())) {
    return 1;
  }

  korora_debug::log(
      "READY transport=1 fairy=3 adelie=2 magellan=1 timer_hz=16000000 "
      "sync_hz=4\r\n");
  while (true) {
    k_sleep(K_SECONDS(60));
  }
  return 0;
}
