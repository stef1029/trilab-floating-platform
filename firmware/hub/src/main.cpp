#include <zephyr/kernel.h>

#include "ble_gateway.hpp"
#include "control.hpp"
#include "debug_log.hpp"
#include "experiment.hpp"
#include "node_manager.hpp"
#include "ttl_generator_manager.hpp"
#include "local_sensors.hpp"
#include "rs485_bus.hpp"
#include "timebase.hpp"

namespace {

bool start_component(const char *name, int result) {
  hub_debug::log("START component=%s status=%d\r\n", name, result);
  return result == 0;
}

} // namespace

int main() {
  if (!start_component("timebase", hub_time::initialize()) ||
      !start_component("rs485", hub_rs485::initialize())) {
    return 1;
  }

  hub_ttl_generator::initialize();
  hub_control::initialize();
  hub_experiment::initialize();

  if (!start_component("nodes", hub_nodes::initialize()) ||
      !start_component("ble", hub_ble::initialize())) {
    return 1;
  }

  // Local sensors deliberately start after BLE so their node telemetry has a
  // transport available immediately. Sensor absence is reported as status and
  // does not make Hub unusable as a communications hub.
  if (!start_component("local_sensors", hub_local_sensors::initialize())) {
    return 1;
  }

  // D3 is driven by nPM1300 LED0 and is reserved as a trustworthy
  // firmware-owned "all startup stages completed" indicator.
  if (!hub_local_sensors::set_ready_led(true)) {
    hub_debug::log("READY_LED unavailable\r\n");
  }

  hub_debug::log(
      "READY transport=1 node=3 host=2 discovery=1 timer_hz=16000000 "
      "sync_hz=4\r\n");
  while (true) {
    k_sleep(K_SECONDS(60));
  }
  return 0;
}
