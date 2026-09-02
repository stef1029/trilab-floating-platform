# Korora firmware

Korora runs on the nRF52840 DK with Zephyr. It is the system time authority, BLE gateway, RS485 master, Fairy inventory owner, and experiment scheduler.

## Responsibilities

- Generate the 4 Hz SYNC pulse from a 16 MHz hardware timer
- Poll up to `FAIRY_MAX_BOARDS` Fairy boards
- Run Magellan discovery and address assignment
- Fit one clock model for every live Fairy
- Connect to exactly one Galapagos over BLE
- Fit the Galapagos BLE anchor clock model
- Advertise the Adelie BLE service
- Route every command through the Adelie protocol
- Route every record through the Fairy protocol
- Serialize all RS485 and BLE writes
- Prioritize Galapagos timing commands and Adelie responses over telemetry
- Schedule TTL trains on Galapagos without a Korora loopback capture
- Stop the active session when Adelie disconnects

## Files

| File                    | Purpose                                               |
| ----------------------- | ----------------------------------------------------- |
| `main.cpp`              | Startup only                                          |
| `timebase.cpp`          | TIMER2 SYNC generation and 16 MHz Korora timebase      |
| `controller_clock.cpp`  | BLE controller clock bridge                           |
| `rs485_bus.cpp`         | COBS, polling, retries, and bus mutex                 |
| `fairy_manager.cpp`     | Discovery, inventory, polling, and Fairy clock models |
| `ble_gateway.cpp`       | Adelie server and Galapagos client                    |
| `galapagos_manager.cpp` | BLE anchor pairing and record conversion              |
| `experiment.cpp`        | Session records, Galapagos TTL train, and sync test    |
| `control.cpp`           | Adelie command routing and local operations           |
| `debug_log.cpp`         | Isolated human serial text                            |

## Pin assignment

The overlay is `boards/nrf52840dk_nrf52840.overlay`.

| Function                     | Pin   |
| ---------------------------- | ----- |
| SYNC output                  | P1.10 |
| RS485 UART TX                | P0.27 |
| RS485 UART RX                | P0.26 |

Korora does not configure an external-event input or a Galapagos TTL loopback input. Galapagos TTLs are scheduled over BLE and their generated records are returned over the protocol.

The prototype RS485 modules provide automatic direction control. No direction GPIO is used.

## Build

From a Zephyr or nRF Connect SDK workspace:

```bash
west build -p always -b nrf52840dk/nrf52840 korora
west flash
```

To build the two Fairy prototype capacity:

```bash
west build -p always -b nrf52840dk/nrf52840 korora -- \
  -DFAIRY_MAX_BOARDS=2
```

The default remains six boards.

## Configuration

`prj.conf` enables:

- C++17
- BLE central and peripheral roles
- Two BLE connections
- 247 byte L2CAP MTU
- Nordic TIMER, RTC, GPIOTE, and GPPI drivers
- Crystal low frequency clock

To disable all human serial output add this build definition:

```text
FAIRY_ENABLE_DEBUG_STREAM=0
```

For a custom board, also merge `debug_off.conf` so Zephyr does not claim a console:

```bash
west build -p always -b YOUR_BOARD korora -- \
  -DFAIRY_ENABLE_DEBUG_STREAM=0 -DEXTRA_CONF_FILE=debug_off.conf
```

`CONFIG_SERIAL` remains enabled because Korora uses UART1 for RS485.

Do not send debug text through the RS485 UART.

## Runtime states

Korora can discover, synchronize, and report health before Adelie connects. It has no experiment storage.

A normal run starts only after:

- Every live UUID is assigned
- Every Fairy clock model is valid
- Galapagos is connected
- The Galapagos clock model is valid

If a Fairy stops responding for three seconds, it leaves the live inventory. If it returns, Magellan restores its address. A changed UUID requires a new Adelie configuration.

## RS485 timing

Assigned Fairies are polled every 10 ms. Poll responses allow 12 ms. This covers a maximum Fairy record at 460800 baud. Failed transactions receive one retry. Absent nodes are probed at a slower rate so one missing board does not occupy the bus.

The bus mutex remains held across the poll request, response, and record acknowledgement. A command or discovery round cannot enter that transaction.
