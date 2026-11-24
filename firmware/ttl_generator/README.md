# TTL generator firmware

TTL generator runs on the nRF54L15 DK with Zephyr. It creates hardware timed TTL pulses and reports its local measurements to the hub.

## Responsibilities

- Advertise one TTL generator BLE service
- Report selected BLE connection anchor observations
- Accept only Command v2 commands from the hub
- Schedule TTL set and clear events with GRTC
- Drive the TTL pin through DPPI and GPIOTE
- Queue up to eight future TTL pulses
- Capture a general digital input with GRTC
- Publish node v3 records
- Force TTL low on disconnect or session stop

## Pin assignment

The overlay is `boards/nrf54l15dk_nrf54l15_cpuapp.overlay`.

| Function            | Pin   |
| ------------------- | ----- |
| General event input | P1.11 |
| TTL output          | P1.12 |

For the current timing test, connect P1.12 to the hub P0.02 and connect the grounds. The output can later connect to the real TTL input without changing the application protocols.

## Files

| File                | Purpose                                     |
| ------------------- | ------------------------------------------- |
| `main.cpp`          | Startup only                                |
| `hardware.cpp`      | GRTC, DPPI, capture, and TTL output         |
| `application.cpp`   | Sessions and Adelie command execution       |
| `record_stream.cpp` | Serialized node and response notifications |
| `ble_service.cpp`   | GATT service and BLE anchor reports         |
| `debug_log.cpp`     | Isolated human serial text                  |

## Build

```bash
west build -p always -b nrf54l15dk/nrf54l15/cpuapp ttl_generator
west flash
```

The build requires an nRF Connect SDK version that provides the SoftDevice Controller anchor report extension and the nRF54L15 GRTC driver.

## TTL rules

- Target time must be aligned to a 1 us GRTC tick
- Target must be at least 50 ms in the future
- Width must be between 1 us and 2000000 us
- Pulses must arrive in increasing target order
- One active pulse and eight queued pulses are supported
- Stop session disables compare events and disconnects TTL DPPI routes

TTL generator emits `TTL generated` from the compare callback. The record contains sequence, requested local time, actual local compare time, and width.

## Debug output

Set `NODE_ENABLE_DEBUG_STREAM=0` as a C++ build definition for a custom PCB. Debug output is not an application link and is never parsed. Merge `debug_off.conf` in a custom build so Zephyr does not claim a console:

```bash
west build -p always -b YOUR_BOARD ttl_generator -- \
  -DNODE_ENABLE_DEBUG_STREAM=0 -DEXTRA_CONF_FILE=debug_off.conf
```
