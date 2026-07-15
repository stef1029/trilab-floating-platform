# Overall architecture - Platform proposal v1

```text
                    CENTRAL HUB
 ┌────────────────────────────────────────────────────┐
 │ nRF52840 + Zephyr                                  │
 │                                                    │
 │ Trial state machine        BLE / USB to host       │
 │ Command scheduler          Raw logging + buffering │
 │                                                    │
 │ Shared oscillator ── divider ── timing counter     │
 │                          │                         │
 │                          ├── differential TIMEBASE │
 │                          ├── differential SYNC     │
 │                          └── TTL sync to DAQ/ephys │
 │                                                    │
 │ RS-485 master ─────────────── data bus             │
 │ Battery protection/charger + regulated rails       │
 └────────────────────────────────────────────────────┘
              │
              │ Power, RS-485, TIMEBASE, SYNC
              │
 ┌────────────────────────────────────────────────────┐
 │                  REWARD-PORT MCU                   │
 │                                                    │
 │ Shared-clock timer                                 │
 │   ├── beam-break hardware capture                  │
 │   ├── scheduled LED output compare                 │
 │   ├── scheduled audio start                        │
 │   └── scheduled valve pulse                        │
 │                                                    │
 │ Event ring buffer + sequence numbers               │
 │ RGB driver|Audio driver|Solenoid MOSFET|Empty Pins │
 └────────────────────────────────────────────────────┘
```


## Decisions TLDR:

* **Hub:** nRF52840 with Zephyr
* **Peripheral:** STM32G031/G071 (32 pin)
* **Timing:** 1 MHz differential shared TIMEBASE plus periodic differential SYNC.
* **Data:** UART over RS485 with CRC and sequence numbers.
* **Power:** regulated 3.3 V logic rail and separate regulated valve rail.
* **Logging:** 64-bit timestamps and persistent sequence counters.
* **Control:** all stimuli and rewards scheduled through timer compare or pre-armed local event rules.

For example:
```text
REWARD-PORT 3:
  cue_start_tick = 18,500,000
  cue_stop_tick  = 18,750,000
  on_valid_beam_break:
    valve_on after 500 us
    valve_off after 25 ms
```

Hub delivers this command structure early, leaving the reward-port MCU with timing execution.

---

# 1. Timing architecture

## Using a shared timebase

TCXO or stable oscillator to generate a **1 MHz shared TIMEBASE**.

Each reward-port MCU runs its processor from its own internal or local clock, but a 32-bit hardware timer counts the externally supplied 1 MHz edges:
* 1 tick = 1 µs
* identical frequency at every module
* much less EMI than distributing 16–32 MHz for internal clock
* no dependency on MCU startup or PLL behavior

## Add a separate SYNC signal

A common clock does **not** create a common counter value. Modules boot at different moments. Generating a periodic SYNC edge at session start and then at 1 Hz should be enough. Each module hardware-captures its local timer when SYNC arrives. The hub knows the corresponding global timestamp.

That gives the mapping:

```text
global_time = local_timer + module_offset
```

Because the frequency is shared, the offset should remain essentially constant.

64 bit software timestamp will work good enough, generated as 32 bit hardware timer with an overflow word. At 1 Mhz overflow time is ~72min, increasing to 64 bit allows for storage of give or take half a century (won't overflow).
  * even an 8 bit extension gives about 300h timekeeping limit

## Clock distribution

Needs a properly designed multidrop bus. Using 1MHz TIMEBASE will be way more stable than 32MHz clock sync.

Sadly nRF52840 RTOS clock sync via bluetooth/radio is not going to be accurate enough :(
  - it also needs access to the timebase as it collects all records

---

# 2. Cable communications

Issues with I2C signal:
* solenoid current edges (integrity)
* PWM LED and audio edges (integrity)
* removable modules (annoying)
* future expansion (also annoying)

## RS485

Using: **half-duplex RS-485**, master-polled by the hub.

Architecture:
```text
Bit rate:          500 kbit/s or 1 Mbit/s
Topology:          daisy-chain trunk around the port ring
Termination:       120 Ω at the two physical ends
Protocol:          hub-master, modules respond only when addressed
Integrity:         CRC-16 or CRC-32
Timeout recovery:  mandatory
```

### Addressing

We can just use autodiscovery features

A nice architecture:
1. Every MCU exposes its factory UID
2. Uncommissioned nodes respond in pseudorandom discovery slots
3. The hub assigns addresses

---

# 3. Peripheral MCU selection

Recommended: **STM32G031K8 or STM32G071K/B in a 32-pin package**.

The STM32G031 family features wanted:
* hardware timer input capture
* timer external-clock inputs
* PWM/output compare
* I2C controller/peripheral support
* USART suitable for RS-485
* watchdog
* external high-speed clock support from 8 to 48 MHz

### Pin budget

| Function                     | GPIO count |
| ---------------------------- | ---------: |
| RS-485 TX, RX, driver-enable |          3 |
| TIMEBASE input               |          1 |
| SYNC capture                 |          1 |
| Beam-break capture           |          1 |
| RGB PWM                      |          3 |
| Audio PWM/data               |          1 |
| Solenoid control             |          2 |
| SWD debug                    |          2 |
| Address/configuration        |        1–3 |
| Status/test output           |          1 |

Absolute minimum is already 16-18 pins so a 32pin package allows for more freedom.

---

# 4. Peripheral event engine

## Input capture

The beam break output feeds:

```text
Photodetector
   ↓
Comparator with hysteresis
   ↓
Timer input-capture channel
   ↓
Capture register / DMA
   ↓
Event ring buffer
```

The timer capture latches the timestamp at the electrical edge. Interrupt latency then does not alter the captured time.

```c
struct port_event {
    uint64_t timestamp_ticks;
    uint32_t sequence;
    uint16_t event_type;
    uint16_t command_id;
    uint32_t data;
};
```

Event type examples:
```text
CUE_ON
CUE_OFF
AUDIO_ON
AUDIO_OFF
VALVE_ON
VALVE_OFF
SYNC_CAPTURE
CLOCK_FAULT
BUFFER_OVERFLOW
```

> as introduced later with programmable conditions, logging must include all actions

## Output scheduling

Use timer output compare instead of firmware delays:
* RGB start/stop at compare timestamps
* audio envelope start at compare timestamp
* valve gate on/off at compare timestamps

## Conditional actions

Codifying local rules can help super low latency actions to be implemented:
```text
IF response_window_active AND module_is_correct_target
THEN valve_on at captured_time + configured_delay
```
* actual code up to be decided/fully firmware level

Of course bidirectional communication is still an option if preferred.

---

# 5. LED design

Three MCU PWM outputs for different colours, but they should drive:
* MOSFET channels with proper current-limiting resistors

Actual onset delay needs to be measured.

---

# 6. Speaker design


### For simple beeps or pure tones

Use:
```text
timer PWM → amplifier or piezo driver → speaker
```

### For noise bursts, clicks, sweeps, or calibrated auditory stimuli

Use:
* high-rate PWM with DMA, reconstruction filter, and amplifier

The peripheral MCU should trigger the waveform from a timer compare, not synthesize it from interrupt timing.

Onset times of sound generation needs to be measured

---

# 7. Solenoid and reward power

## Required protection

Each valve driver should include:
* gate pulldown so the valve stays off during reset
* logic-level MOSFET
* flyback suppression
* local bulk capacitance
* current return path directly to the power entry
* maximum on time safety cutoff (firmware)
* watchdog behavior that forces the valve off

Effects on speed of valve operation need to be studied.

## Battery voltage is not a 4.3 V rail

Driving a valve directly from the battery will cause its force and opening dynamics to change with state of charge. Use:
```text
Battery
 ├── 3.3 V buck/buck-boost → MCU, sensor, logic
 └── regulated valve rail → solenoid and possibly audio
```

Valve rail needs to be adjusted with voltage/current depending on valve design and activation

Beam break only partly proves that a valve opened, do we need something better (?)

---

# 8. Cable definition

Multidrop ring:
```text
Pair 1: RS-485 A / B
Pair 2: TIMEBASE+ / TIMEBASE-
Pair 3: SYNC+ / SYNC-
Pair 4: POWER / GND
```

Would be nice to have separate power lines for each peripheral unit but this works too.

At each module:

* TVS protection on externally exposed digital pairs
* local decoupling
* bulk capacitance near the valve driver
* logic and valve return routing that rejoins only near power entry
* test points for TIMEBASE, SYNC, beam, valve gate, and 3.3 V

---

# 9. Hub design

The nRF52840 hub should own:
* protocol and trial configuration
* future-timestamp command scheduling
* module health polling
* event collection
* sequence-gap detection
* BLE telemetry

## Syncing with other components

Using multiple sync messages between the hub and the computer an offset between computer time and hub timestamp time can be calculated
  - sync messages happen in  between trials as well

## Position tracking

?

