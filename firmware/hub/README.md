# Hub firmware

Hub runs with Zephyr on the Floating Hub PCB (BMD-340 module, built as `ubx_bmd340eval/nrf52840`) or on the nRF52840 DK breadboard rig. It is the system time authority, BLE gateway, RS485 master, node inventory owner, and experiment scheduler.

## Responsibilities

- Generate the 4 Hz SYNC pulse from a 16 MHz hardware timer
- Poll up to `RIG_MAX_NODES` node boards
- Run discovery and address assignment
- Fit one clock model for every live node
- Connect to exactly one TTL generator over BLE
- Fit the TTL generator BLE anchor clock model
- Advertise the Adelie BLE service
- Route every command through the Command protocol
- Route every record through the Record protocol
- Serialize all RS485 and BLE writes
- Prioritize TTL generator timing commands and Adelie responses over telemetry
- Schedule TTL trains on TTL generator without a Hub loopback capture
- Stop the active session when Adelie disconnects

## Files

| File                    | Purpose                                               |
| ----------------------- | ----------------------------------------------------- |
| `main.cpp`              | Startup only                                          |
| `timebase.cpp`          | TIMER2 SYNC generation and 16 MHz Hub timebase      |
| `controller_clock.cpp`  | BLE controller clock bridge                           |
| `rs485_bus.cpp`         | COBS, polling, retries, and bus mutex                 |
| `node_manager.cpp`     | Discovery, inventory, polling, and node clock models |
| `ble_gateway.cpp`       | Adelie server and TTL generator client                    |
| `ttl_generator_manager.cpp` | BLE anchor pairing and record conversion              |
| `experiment.cpp`        | Session records, TTL generator TTL train, and sync test    |
| `control.cpp`           | Adelie command routing and local operations           |
| `debug_log.cpp`         | Isolated human serial text                            |

## Pin assignment

The Floating Hub PCB overlay is `boards/ubx_bmd340eval_nrf52840.overlay`. The nRF52840 DK breadboard overlay is `boards/nrf52840dk_nrf52840.overlay`.

| Function                     | Hub PCB | DK    |
| ---------------------------- | ------- | ----- |
| SYNC output                  | P1.10   | P1.10 |
| SYNC transmitter DE and /RE  | P0.16   | none  |
| RS485 UART TX                | P0.19   | P0.27 |
| RS485 UART RX (pulled up)    | P0.17   | P0.26 |
| RS485 DE and /RE             | P0.15   | P1.12 |

Hub does not configure an external-event input or a TTL generator TTL loopback input. TTL generator TTLs are scheduled over BLE and their generated records are returned over the protocol.

On the hub PCB the RS485 transceiver has DE and /RE tied to one direction pin. Hub holds it low to listen, raises it only while UARTE is sending, and releases it in the `UART_TX_DONE` callback so a node can reply straight away. RX has a pull-up because the transceiver's receiver output is high impedance while the pin is high. The dedicated SYNC transmitter is held in transmit mode for the lifetime of the timebase.

The DK rig uses Grove RS485 modules with automatic direction control, so nothing is wired to its direction pin. P1.12 is an otherwise unused DK pin, and the firmware's direction writes go nowhere.

## Build

From a Zephyr or nRF Connect SDK workspace:

```bash
west build -p always -b ubx_bmd340eval/nrf52840 hub   # hub PCB
west build -p always -b nrf52840dk/nrf52840 hub       # DK rig
west flash
```

On Windows, `firmware/nordic.cmd` wraps these with the right board and build directory.

To build the two node prototype capacity:

```bash
west build -p always -b nrf52840dk/nrf52840 hub -- \
  -DNODE_MAX_BOARDS=2
```

The default is five boards.

## Configuration

`prj.conf` enables:

- C++17
- BLE central and peripheral roles
- Two BLE connections
- 247 byte L2CAP MTU
- Nordic TIMER, RTC, GPIOTE, and GPPI drivers
- Calibrated RC low frequency clock (the hub PCB has no 32.768 kHz crystal)

To disable all human serial output add this build definition:

```text
NODE_ENABLE_DEBUG_STREAM=0
```

For a custom board, also merge `debug_off.conf` so Zephyr does not claim a console:

```bash
west build -p always -b YOUR_BOARD hub -- \
  -DNODE_ENABLE_DEBUG_STREAM=0 -DEXTRA_CONF_FILE=debug_off.conf
```

`CONFIG_SERIAL` remains enabled because Hub uses UART1 for RS485.

Do not send debug text through the RS485 UART.

## Runtime states

Hub can discover, synchronize, and report health before Adelie connects. It has no experiment storage.

A normal run starts only after:

- Every live UUID is assigned
- Every node clock model is valid
- TTL generator is connected
- The TTL generator clock model is valid

If a node stops responding for three seconds, it leaves the live inventory. If it returns, Discovery restores its address. A changed UUID requires a new Adelie configuration.

## RS485 timing

Assigned Nodes are polled every 10 ms. Poll responses allow 12 ms. This covers a maximum record at 460800 baud. Failed transactions receive one retry. Absent nodes are probed at a slower rate so one missing board does not occupy the bus.

The bus mutex remains held across the poll request, response, and record acknowledgement. A command or discovery round cannot enter that transaction.
