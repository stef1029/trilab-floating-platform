# Hub firmware

Hub runs with Zephyr on the Floating Hub PCB (BMD-340 module, built as `ubx_bmd340eval/nrf52840`) or on the nRF52840 DK breadboard rig. It is the system time authority, BLE gateway, RS485 master, node inventory owner, and experiment scheduler.

## Responsibilities

- Generate the 4 Hz SYNC pulse from a 16 MHz hardware timer
- Poll up to `RIG_MAX_NODES` node boards
- Find which node boards are present by polling their fixed addresses
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
| `node_manager.cpp`     | Presence, inventory, polling, and node clock models |
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

Hub can find nodes, synchronize, and report health before Adelie connects. It has no experiment storage.

A normal run starts only after:

- At least one node is present (Adelie decides which nodes a run needs)
- Every node clock model is valid
- TTL generator is connected
- The TTL generator clock model is valid

Every node board is given a number, 0 to `RIG_MAX_NODES - 1`, when it is flashed (`firmware/reward_port/scripts/flash.py --id N`), and node N always uses address `0x10 + N`. There is no discovery: Hub polls every slot from boot, so it does not matter whether Hub or the nodes power up first. If a node stops responding for three seconds, it leaves the live inventory and a running session stops; when it answers again it rejoins. A node that resets is noticed from its boot record, which also stops a running session and restarts its clock model. The board's UUID is reported for information only, and stays zero for a board that was already running when Hub started until that board next resets.

## RS485 timing

Assigned Nodes are polled every 10 ms. Poll responses allow 12 ms. This covers a maximum record at 460800 baud. Failed transactions receive one retry. Absent slots are asked every 500 ms whether a board is there, so a missing board costs about 2% of the bus.

The bus mutex remains held across the poll request, response, and record acknowledgement. A command cannot enter that transaction.
