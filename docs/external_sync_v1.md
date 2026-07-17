# Overall Architecture — External DAQ sync v1

- A moving **nRF52840 hub** owns the platform time base, trial state, event collection, buffering, and direct BLE communication with the main computer.
- A second, stationary **nRF52840 timing bridge** sits beside the DAQ. It maintains a hardware-clock relationship with the moving hub over BLE and generates future-timestamped TTL synchronization markers for the DAQ.
- The DAQ records TTL edges in its own sample-clock domain and sends its data to the computer over USB.
- Behavioural event data remains direct, the stationary bridge is not a data relay.

```text
                               STATIONARY SIDE
                      ┌──────────────────────────┐
                      │ Main computer            │
                      │                          │
                      │ Platform event receiver  │
                      │ DAQ recording software   │
                      │ Alignment and storage    │
                      └──────^───────────^───────┘
                             │ BLE       │ USB
                             │           │
          MOVING PLATFORM    │           │
 ┌───────────────────────────┴────┐   ┌──┴─────────────────┐
 │ CENTRAL HUB — nRF52840         │   │ External DAQ/ephys │
 │                                │   │                    │
 │ Trial state machine            │   │ Neural/analogue    │
 │ Command scheduler              │   │ acquisition clock  │
 │ Raw logging and buffering      │   │ Digital TTL input  │
 │ Direct BLE data to computer    │   └────────^───────────┘
 │                                │            │ TTL
 │ Platform timing authority      │   ┌────────┴───────────┐
 │   ├── 1 MHz TIMEBASE           │   │ Stationary nRF52840│
 │   ├── periodic SYNC            │   │ timing bridge      │
 │   └── 64-bit platform time     │   │                    │
 │                                │<->│ BLE clock mapping  │
 │ RS-485 master                  │   │ TTL OUT / TTL IN   │
 │ Battery and regulated rails    │   │                    │
 └──────────────┬─────────────────┘   └────────────────────┘
                │
                │ Power, RS-485, TIMEBASE, SYNC
                │ Electrically linear trunk
                │
 ┌──────────────v────────────────────────────────────────┐
 │ REWARD-PORT MCU — STM32G071-class                     │
 │                                                       │
 │ Shared-time timer                                     │
 │   ├── beam-break hardware capture                     │
 │   ├── scheduled RGB output compare                    │
 │   ├── scheduled audio start                           │
 │   └── scheduled valve pulse                           │
 │                                                       │
 │ Event ring buffer and sequence IDs                    │
 │ RGB driver | audio driver | solenoid MOSFET | spares  │
 └───────────────────────────────────────────────────────┘
```

## Decisions — TLDR

- **DAQ data path —** DAQ has it’s own data path and time sync with the computer, does not interact with floating platform event system
- **Added MCU —** DAQ gets a small BLE capable MCU (like nRF52840) which will be used for TTL generation
- **Future stamped BLE TTL generation —** Floating platform chip can time sync with the DAQ MCU to ~4us, sending a future TTL signal with a known timestamp gives a generated TTL for the DAQ on the order of 5-10us

## Assumptions about the existing system

- **Floating platform hub —** An nRF52840 (or equivalent) based hub collects timestamped records from all reward ports and owns platform time. It supports direct BLE communication to the main computer and a second BLE timing connection to a stationary bridge.
- **External DAQ —** The DAQ records TTL transitions against its hardware sample clock and reports its acquired data to the computer over USB. Its input and sample quantization error is represented by $e_d$
- **Computer —** The computer stores both streams and performs alignment. Operating system BLE or USB packet arrival time is not treated as the experimental timestamp.
- **Stationary timing bridge —** A stationary nRF52840 (or equivalent) near the DAQ maps its hardware timer to the moving hub timer and converts scheduled platform timestamps into hardware TTL edges

---

# 1. External DAQ and electrophysiology synchronization

## Architecture

The moving platform remains untethered. A stationary nRF52840 timing bridge is installed beside the external DAQ:

```text
MOVING HUB                                  STATIONARY RIG

platform_tick H
event buffering ───── BLE direct ─────────► computer
      │
      └──── BLE clock synchronization ────► stationary nRF
                                             │
                                             ├── TTL OUT → DAQ digital input
                                             ├── TTL IN  ← DAQ/laser output
                                             └── USB diagnostics → computer

DAQ/ephys ───────────── USB/data link ─────► computer
```

## Two BLE connections

The moving hub supports two concurrent BLE links:

### Link A — hub to computer

The computer acts as Central, The moving hub acts as Peripheral and exposes experiment-data and control services.

Traffic includes:
- reward-port events
- IMU records
- executed cue and reward events
- sequence IDs
- health information
- trial configuration and commands

### Link B — hub to stationary timing bridge

The moving hub acts as Central. The stationary bridge acts as Peripheral.

Traffic is low volume:
- connection-anchor timing information
- clock-quality updates
- future synchronization-marker schedules
- pulse IDs and widths
- bridge status
- execution acknowledgements

## BLE clock synchronization method

The selected baseline is Nordic connection time synchronization over a normal BLE ACL connection.

Both devices observe BLE connection event anchor points in their own local clock domains. The moving hub sends its anchor reference to the stationary bridge. The bridge estimates:

$S = a_{hs}H + b_{hs}$

Nordic's sample demonstrates microsecond scale synchronized timed triggers on supported hardware, including nRF52840 development kits. This is a reference implementation but it serves well to show target scaling of errors.

Bluetooth ISO time synchronization remains an alternative if one source must synchronize several independent receivers or if ISO transport is otherwise needed. It is not required for a single stationary bridge.

## Future timestamped TTL markers

Precision synchronization should use a future timestamp, not a “toggle when this BLE packet arrives” command.

Example:
```text
SYNC_SCHEDULE:
  sync_id          = 105
  hub_trigger_tick = 80,000,000
  pulse_width_us   = 1,000
```

The stationary bridge:
1. converts `hub_trigger_tick` into stationary time
2. verifies that the command arrived before the minimum arm deadline
3. programs a hardware timer compare
4. uses timer/PPI/GPIOTE to toggle the output without an RTOS callback
5. records scheduled and actual local trigger timestamps
6. reports success or failure

The BLE message may arrive with variable latency. That latency does not affect the TTL edge if the marker is armed in time.

A `FIRE_NOW` command may be retained for wiring tests and latency measurements but shouldn't be used as the scientific synchronization reference.

## DAQ recording and USB transport

The DAQ records the TTL transition against its own hardware sample clock:
```text
sync_id 105:
  hub timestamp = H105
  DAQ sample     = D105
```

USB transports the already acquired DAQ data to the computer. USB buffer or callback latency does not alter the DAQ sample index if timestamping occurs in DAQ hardware.

With repeated synchronization pairs:
$D_k = \alpha H_k + \beta$

the computer estimates the offset and relative clock rate between platform time and DAQ time. A platform event at timestamp \(H_e\) is translated to:
$D_e = \alpha H_e + \beta$

Coeffs of the linear transformation should stay stable throughout the experimental runtime.

## Marker identification

Every marker should have a monotonically increasing `sync_id`.

Regular anonymous 1 Hz pulses can become ambiguous after:
- a missed pulse
- DAQ restart
- BLE reconnect
- partial recording

Recommended approach:
- regular timing pulses
- periodic distinctive barcode bursts or pseudo-random intervals
- explicit marker IDs in hub and bridge logs
- start of session and reconnection epoch markers

## Bidirectional TTL

The bridge should provide:
```text
SYNC_OUT -> DAQ digital input
SYNC_IN  <- DAQ or electrophysiology digital output
```

This supports:
- hub-master synchronization
- DAQ-master synchronization
- additional validation
- measurement of output and input path delay
- compatibility with systems that already expose sync clocks

## Optic control

The stationary bridge may provide a separate TTL output for laser control. The laser event is known in advance and can be armed at an absolute platform timestamp. This is compatible with hardware timer scheduling and deterministic onset.

(maybe not needed at all)

---

# 2. Validation plan

## Internal timing validation

Using an oscilloscope to compare:
- hub TIMEBASE at source
- TIMEBASE at first, middle, and final reward ports
- SYNC edges at all modules
- scheduled output edges from multiple modules
- captured beam-test edges

Measuring:
- propagation delay
- channel-to-channel skew
- jitter
- missed edge rate
- offset stability over temperature and full session duration

## Wireless bridge validation

Generating future timestamped outputs on both hub and stationary bridge and observe them simultaneously. We test:
- normal radio conditions
- maximum event traffic
- two active BLE connections
- BLE retransmissions
- reconnection
- multi hour drift
- minimum and maximum connection intervals under consideration

## End-to-end DAQ validation

During development, we produce synthetic sync events. Fit the clock map and calculate residual error.

Provisional acceptance targets:
- no clock discontinuities
- no unidentified marker loss
- no systematic error growth through a multi-hour session
- 95th-percentile absolute alignment error ≤0.5 ms
- 99th-percentile absolute alignment error ≤1.0 ms
