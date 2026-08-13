# Korora Hub

Central nRF52840-based hub board for the behavioural rig. Korora provides:

- RS-485 master communication to peripheral boards
- Differential SYNC distribution to peripheral boards
- System time authority and synchronization
- 9-DoF motion/orientation sensing
- LiPo battery charging and power-path management
- Battery / power telemetry to the nRF52840
- 3V3 and VLOAD distribution over the peripheral harness
- USB/SWD-style development and debug access as appropriate for the final PCB

The current controller module is the BMD-340-A-R, based on the nRF52840.

The current power-management assumption is an nPM1300-class Nordic PMIC. Exact PMIC variant, battery connector, thermistor arrangement, charge current, and rail configuration must be finalized against the selected battery before schematic release.

## System architecture

Korora is the system master.

The hub:

- owns the system timebase
- initiates RS-485 transactions
- emits the differential SYNC signal
- gathers peripheral event timestamps
- translates peripheral-local timestamps into hub/system time
- acquires the local IMU and magnetometer
- runs or hosts the 9-DoF orientation-fusion algorithm
- monitors battery and power state
- distributes 3V3, VLOAD, and ground to the peripheral chain

Initial high-level architecture:

```text
                         +----------------------+
5V BENCH INPUT --------->|                      |
                         |       nPM PMIC       |
LiPo BATTERY ----------->| charger / fuel gauge|
                         | power-path / bucks   |
                         +----+-------------+---+
                              |             |
                             3V3           VSYS
                              |             |
                              |             +---------- VLOAD
                              |                         |
                              +-------------------------+--> JST-GH
                              |
                    +---------+---------+
                    |    BMD-340-A-R    |
                    |      nRF52840     |
                    +----+----+----+----+
                         |    |    |
                      UART   GPIO  I2C/SPI
                         |    |    |
                         |    |    +----> IMU
                         |    |    +----> magnetometer
                         |    |    +----> PMIC telemetry
                         |    |
                         |    +---------> SN65HVD75 #2
                         |                  SYNC_P/N
                         |
                         +-------------> SN65HVD75 #1
                                            RS485_P/N
```

## Relationship to peripheral boards

Korora is designed to connect directly to peripheral boards using the common 10-pin JST-GH bus.

Korora is the RS-485 bus master and the source of the differential SYNC pulse. Peripheral boards are expected to:

- listen for hub commands
- transmit only when addressed
- capture SYNC locally in hardware
- report captured timestamps and local events back to Korora

The initial Reward Port design assumes a 500000 baud, 8-N-1, half-duplex RS-485 link and an initially 4 Hz SYNC pulse.

These values are initial protocol parameters rather than permanent electrical limitations.

## Power inputs

Korora has two power sources:

1. LiPo battery
2. External 5 V bench / development supply

The PMIC provides charging and power-path control between them.

### Battery input

The earlier system requirement described the battery as approximately 4.3 V.

Do not set the PMIC charge termination voltage from that description alone. The final battery datasheet controls:

- maximum charge voltage
- charge current
- temperature limits
- NTC requirements
- minimum discharge voltage
- protection requirements

### Bench power input

Provide a 2-pin header for development power:

| Pin | Signal    |
| --: | --------- |
|   1 | +5V_BENCH |
|   2 | GND       |

The bench input feeds the PMIC input rather than directly feeding the battery rail.

Intended behaviour:

```text
5 V bench present
        |
        v
      PMIC
      /  \
     /    +----> system load
    |
    +---------> battery charging
```

The bench supply must be current limited during initial bring-up.

## Power rails

Korora distributes two positive rails to the peripheral connector.

### 3V3

3V3 is the regulated digital rail.

Expected loads include:

- BMD-340-A-R
- RS-485 transceivers
- local sensors
- PMIC digital interface as required
- peripheral-board 3V3 loads through the JST-GH chain

The total required 3V3 current is TBD.

Before finalizing the PMIC configuration, calculate:

```text
hub local 3V3 load
+
maximum number of peripheral boards
x
maximum peripheral 3V3 load
=
required regulated 3V3 current
```

If this exceeds the useful continuous current available from the selected PMIC regulator configuration, Korora requires an external 3V3 regulator or a revised power architecture.

### VLOAD

VLOAD is currently defined as the PMIC system / VSYS rail.

VLOAD is not assumed to be a precision regulated 5 V rail.

Conceptually:

```text
battery operation:
battery -> PMIC power path -> VSYS -> VLOAD

bench operation:
5 V input -> PMIC power path -> VSYS -> VLOAD
```

Therefore VLOAD may vary with power-source state and battery condition.

Peripheral loads must be designed for the specified VLOAD operating range.

The final VLOAD voltage range and current limit must be documented after the PMIC and battery design are finalized.

## PMIC interface

Korora should provide a digital control / telemetry link between the PMIC and the nRF52840.

Current intended interface:

```text
BMD-340 SDA <------> PMIC SDA
BMD-340 SCL -------> PMIC SCL
PMIC IRQ ----------> BMD-340 GPIO
```

The firmware should be able to query and log, where supported by the selected PMIC configuration:

- battery voltage
- battery current
- input-source state
- charging state
- state of charge
- battery health / aging information
- PMIC fault state
- regulator state
- power-loss or low-battery warnings

Exact registers and reported metrics depend on the final PMIC implementation.

## Main controller

Controller module:

```text
BMD-340-A-R
nRF52840
```

Korora uses the nRF52840 for:

- system timing
- RS-485 protocol
- SYNC generation
- peripheral discovery and polling
- event collection
- sensor acquisition
- sensor fusion
- battery telemetry
- BLE / radio functionality if required
- diagnostics
- firmware update / development functions

## BMD-340 pin assignment

Physical BMD-340 pin numbers are not yet frozen.

Reserve logical functions for:

| Function             | Peripheral / type          | Notes                      |
| -------------------- | -------------------------- | -------------------------- |
| RS-485 transmit      | UARTE TX                   | Main peripheral bus        |
| RS-485 receive       | UARTE RX                   | Main peripheral bus        |
| RS-485 driver enable | GPIO or UARTE DE strategy  | Must default bus-safe      |
| SYNC output          | GPIO / timer-driven output | Feeds second RS-485 driver |
| PMIC SDA             | TWIM / I2C                 | Shared bus possible        |
| PMIC SCL             | TWIM / I2C                 | Shared bus possible        |
| PMIC interrupt       | GPIO input                 | Optional but recommended   |
| IMU bus              | SPI or I2C                 | Final choice TBD           |
| IMU interrupt 1      | GPIO input                 | Data-ready / FIFO event    |
| IMU interrupt 2      | GPIO input                 | Optional                   |
| Magnetometer bus     | I2C or SPI                 | Final choice TBD           |
| Magnetometer DRDY    | GPIO input                 | Optional but recommended   |
| SWDIO                | SWD                        | Debug                      |
| SWDCLK               | SWD                        | Debug                      |
| RESET                | reset                      | Debug / recovery           |

The final pin assignment should be chosen only after confirming:

- BMD-340 pin availability
- nRF52840 peripheral routing
- high-frequency / antenna placement constraints
- PCB routing
- sensor interrupt requirements
- boot and debug requirements

## RS-485 data bus

Korora uses one SN65HVD75DGKR for the main half-duplex RS-485 bus.

Logical connections:

```text
BMD UARTE_TX ---------> SN65HVD75 D
BMD UARTE_RX <--------- SN65HVD75 R
BMD RS485_DE ---------> SN65HVD75 DE
                       SN65HVD75 /RE
                              |
                              +---- RS485_P
                              +---- RS485_N
```

Exact DE and /RE wiring is TBD.

The design must guarantee that Korora does not unintentionally drive the bus during reset or boot.

Initial protocol configuration inherited from the first Reward Port:

```text
500000 baud
8 data bits
no parity
1 stop bit
half duplex
hub master polling
peripherals transmit only when addressed
```

Korora is the only normal bus master.

## Differential SYNC output

Korora uses a second SN65HVD75DGKR as a dedicated differential digital-output driver.

Concept:

```text
BMD SYNC GPIO --------> SN65HVD75 D
                             |
                             +---- SYNC_P
                             +---- SYNC_N
```

Korora is the source of SYNC.

Peripheral boards receive the differential pair and convert it back to a local logic signal for hardware timer capture.

Initial SYNC frequency:

```text
4 Hz
```

The exact pulse width is TBD.

The SYNC waveform should be generated from a hardware timer or hardware-timed GPIO mechanism where practical so that edge timing does not depend on normal firmware scheduling latency.

SYNC_P / SYNC_N provide differential signalling and noise immunity.

They do not by themselves provide authentication or cryptographic security.

## System time

Korora is the system time authority.

The design currently assumes:

- no distributed high-frequency TIMEBASE line
- peripheral boards keep their own local hardware timers
- Korora periodically emits SYNC
- peripherals hardware-capture the SYNC edge
- peripherals report the captured local timestamp
- Korora estimates the mapping between peripheral-local time and hub time

Korora should maintain a monotonic high-resolution local clock suitable for:

- command timestamps
- peripheral event timestamps
- sensor timestamps
- SYNC generation
- logging
- session-relative time

## Daisy-chain connector

Korora uses the same 10-pin JST-GH pinout as the peripheral boards.

| Pin | Signal  |
| --: | ------- |
|   1 | RS485_P |
|   2 | RS485_N |
|   3 | SYNC_P  |
|   4 | SYNC_N  |
|   5 | 3V3     |
|   6 | GND     |
|   7 | VLOAD   |
|   8 | GND     |
|   9 | VLOAD   |
|  10 | GND     |

Pins 7 and 9 are the same VLOAD net.

Pins 6, 8, and 10 join the same ground system.

Korora will normally be one physical end of both differential buses.

Therefore the hub should include termination provisions for:

- RS485_P to RS485_N
- SYNC_P to SYNC_N

Preferred implementation:

```text
optional 120 ohm RS-485 termination
optional 120 ohm SYNC termination
```

using solder jumpers or clearly defined assembly options.

Only the physical ends of each differential bus should be terminated.

## Connector current distribution

The duplicated VLOAD and GND pins are intended to reduce harness resistance and increase available load-current capacity.

Before schematic release, define:

- JST-GH contact current assumptions
- cable wire gauge
- cable length
- maximum number of daisy-chained peripherals
- maximum simultaneous peripheral current
- VLOAD transient current
- allowed voltage drop
- ground-return distribution

Do not use connector nominal current ratings alone as the system current budget.

The complete cable and connector chain must be checked.

## Local 9-DoF sensing

Korora contains:

```text
6-axis IMU:
STEVAL-MKI240KA during development
LSM6DSV32X sensor

3-axis magnetometer:
MMC5983MA / MMC5983MA-B development hardware
```

Combined measurements:

```text
accelerometer:  X Y Z
gyroscope:      X Y Z
magnetometer:   X Y Z

total: 9 axes
```

Initial requested fused update rate:

```text
20 Hz to 100 Hz
```

This is the output / processing rate target, not necessarily the raw sensor sampling rate.

Raw accelerometer and gyroscope data may be sampled faster and decimated or fused into a 20-100 Hz application output.

## Sensor interface

Final electrical bus allocation is TBD.

Two reasonable starting configurations are:

```text
Option A:
IMU           -> SPI
magnetometer  -> I2C
PMIC          -> I2C

Option B:
IMU           -> I2C
magnetometer  -> I2C
PMIC          -> I2C
```

Option A gives the IMU a dedicated high-throughput bus.

Option B minimizes routing and is likely sufficient for the initial 20-100 Hz application rate.

During prototype bring-up, use the interfaces exposed by the available evaluation boards.

The final integrated PCB should use the sensor IC interfaces directly unless there is a deliberate reason to retain plug-in modules.

## Sensor interrupts

Prefer hardware data-ready interrupts rather than polling for timing-sensitive sensor acquisition.

Recommended signals:

```text
IMU INT1 -------> BMD GPIO
IMU INT2 -------> BMD GPIO    optional
MAG DRDY -------> BMD GPIO    if available / useful
```

Firmware should timestamp sensor-ready events against the same hub timebase used for the rest of the system.

## Sensor fusion

The initial 9-DoF processing goal is orientation tracking.

Expected outputs may include:

- orientation quaternion
- roll
- pitch
- yaw / magnetic heading
- angular velocity
- linear acceleration estimate
- sensor quality / calibration state

Candidate fusion approaches include:

- Madgwick
- Mahony
- complementary-filter variants
- extended Kalman filter

The exact algorithm is TBD.

The first implementation should prioritize:

- stable timestamps
- deterministic sample cadence
- accelerometer calibration
- gyroscope bias calibration
- magnetometer hard-iron calibration
- magnetometer soft-iron calibration
- sensor-axis alignment

## Position-tracking limitation

The 9-DoF sensor stack does not by itself provide stable long-term absolute XYZ position.

Double integration of accelerometer error causes position drift.

Therefore the current hub requirements should distinguish between:

```text
orientation / motion tracking
```

and:

```text
absolute or long-term XYZ position tracking
```

If absolute position is later required, Korora will need an external position reference such as:

- UWB
- optical tracking
- GNSS for suitable outdoor use
- fixed beacons
- application-specific mechanical constraints

This is a system-level requirement and should be resolved before relying on the IMU as a position sensor.

## Sensor placement

The IMU and magnetometer require deliberate PCB placement.

### IMU

Place the IMU:

- on mechanically stable PCB area
- away from board edges that flex
- away from high-vibration switching components where practical
- with a documented sensor-to-board axis convention

Mark the sensor axes in the schematic notes and PCB documentation.

### Magnetometer

The magnetometer is sensitive to local magnetic fields.

Keep it as far as practical from:

- inductors
- high-current VLOAD traces
- battery-current loops
- speakers
- solenoids
- ferromagnetic fasteners
- large copper current loops
- switching regulators

The preferred magnetometer location is likely near a PCB edge with a controlled keepout from magnetic / high-current components.

Final placement should be validated experimentally.

## Antenna placement

The BMD-340-A-R antenna area must be treated as an RF keepout.

Final PCB layout must follow the module manufacturer's antenna-placement requirements.

In particular, do not allow the power section, battery, magnetometer, high-current harness routing, or large copper structures to compromise the antenna region.

Place the BMD-340 near a suitable board edge unless the final RF design intentionally uses a different approved arrangement.

## Debug and programming

Provide access to at least:

```text
SWDIO
SWDCLK
RESET
3V3 reference
GND
```

Preferred implementation is a compact Tag-Connect footprint, pogo-pad pattern, or small debug header.

Also provide useful test points for:

- 5V_BENCH
- battery positive
- VSYS / VLOAD
- 3V3
- RS485_DE
- UART TX
- UART RX
- SYNC logic signal
- PMIC interrupt
- sensor interrupt lines

## Firmware responsibilities

Korora firmware is responsible for:

- boot-time safe-state configuration
- PMIC initialization
- battery monitoring
- monotonic system time
- differential SYNC generation
- peripheral discovery
- RS-485 master arbitration
- command transmission
- response validation
- peripheral clock translation
- event aggregation
- IMU acquisition
- magnetometer acquisition
- sensor calibration
- sensor fusion
- local logging / diagnostics
- radio or host communication if enabled
- watchdog handling
- low-battery behaviour

## RS-485 master behaviour

Initial bus behaviour:

1. Korora owns the bus.
2. Korora addresses one peripheral.
3. The addressed peripheral may respond.
4. Unaddressed peripherals remain receive-only.
5. Korora detects response timeout or malformed response.
6. Korora continues polling according to the active session configuration.

The protocol still needs definitions for:

- peripheral address format
- discovery
- message framing
- CRC
- command IDs
- session IDs
- timestamp format
- timeout
- retries
- fault reporting
- firmware / hardware version reporting

## SYNC behaviour

Initial sequence:

1. Start the hub monotonic clock.
2. Configure SYNC output hardware.
3. Emit SYNC at the configured cadence.
4. Record the hub timestamp associated with each emitted edge.
5. Poll peripherals for their captured local SYNC timestamps.
6. Estimate local-to-hub clock offset and rate.
7. Mark a peripheral unsynchronized when the estimate becomes invalid.

The first peripheral README assumes an initial 4 Hz SYNC rate.

The final rate should be chosen from measured oscillator drift, event-timing requirements, bus traffic, and desired recovery time after connection.

## Battery and low-power behaviour

Low-battery policy is TBD.

Firmware should eventually define thresholds for:

- warning
- reduced functionality
- peripheral VLOAD shutdown if hardware supports it
- radio behaviour
- safe session termination
- final shutdown

Do not allow an uncontrolled battery collapse to be the normal session-ending mechanism.

## Fault handling

Korora should detect and log at least:

- PMIC fault
- low battery
- charger fault
- missing sensor
- IMU communication fault
- magnetometer communication fault
- invalid sensor calibration
- RS-485 timeout
- RS-485 framing / CRC error
- peripheral missing
- peripheral synchronization loss
- watchdog reset
- brownout reset

Where useful, store reset cause and fault state across reboot.

## Startup order

Recommended initial startup order:

1. Keep RS-485 driver disabled.
2. Keep SYNC output in a defined inactive state.
3. Configure clocks and the hub monotonic timer.
4. Initialize watchdog and reset-cause logging.
5. Initialize PMIC communication.
6. Verify 3V3 and power-source state.
7. Initialize IMU communication.
8. Initialize magnetometer communication.
9. Load or initialize sensor calibration.
10. Initialize RS-485 UART and driver-enable control.
11. Start SYNC generation.
12. Discover peripheral boards.
13. Establish synchronization state.
14. Start sensor acquisition and fusion.
15. Enter the normal polling / session loop.

The system must not allow accidental RS-485 transmission while the nRF52840 pins are still in reset/default state.

## Shutdown behaviour

Recommended controlled shutdown sequence:

1. Stop issuing new peripheral actions.
2. Tell peripherals to enter safe state where applicable.
3. Flush critical logs.
4. Mark session end.
5. Stop or disable VLOAD if the final hardware supports switched VLOAD.
6. Configure wake sources.
7. Enter low-power mode or PMIC-controlled shutdown.

Exact behaviour is TBD.

## Open hardware decisions

The following items are not yet frozen:

- exact nPM PMIC part / package
- exact LiPo cell
- battery connector
- NTC implementation
- charge current
- 3V3 total current requirement
- whether an external 3V3 regulator is required
- whether VLOAD is always-on VSYS or switchable
- maximum VLOAD current
- number of supported peripheral boards
- exact BMD-340 GPIO mapping
- RS-485 DE / /RE wiring
- RS-485 and SYNC protection components
- SYNC pulse width
- sensor bus selection
- whether eval boards are only prototypes or remain pluggable
- final IMU part implementation
- final magnetometer part implementation
- antenna / enclosure constraints
- debug connector style
- host / USB interface requirements
