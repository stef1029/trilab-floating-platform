#include <zephyr/kernel.h>

#include "ble_gateway.hpp"
#include "control.hpp"
#include "debug_log.hpp"
#include "experiment.hpp"
#include "fairy_manager.hpp"
#include "galapagos_manager.hpp"
#include "local_sensors.hpp"
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

  // Local sensors deliberately start after BLE so their Fairy telemetry has a
  // transport available immediately. Sensor absence is reported as status and
  // does not make Korora unusable as a communications hub.
  if (!start_component("local_sensors", korora_local_sensors::initialize())) {
    return 1;
  }

  // D3 is driven by nPM1300 LED0 and is reserved as a trustworthy
  // firmware-owned "all startup stages completed" indicator.
  if (!korora_local_sensors::set_ready_led(true)) {
    korora_debug::log("READY_LED unavailable\r\n");
  }

  korora_debug::log(
      "READY transport=1 fairy=3 adelie=2 magellan=1 timer_hz=16000000 "
      "sync_hz=4\r\n");
  while (true) {
    k_sleep(K_SECONDS(60));
  }
  return 0;
}
