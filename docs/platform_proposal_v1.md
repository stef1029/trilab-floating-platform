# Overall architecture - Platform proposal v1

- A moving **nRF52840 hub** owns the platform time base, trial state, event collection, buffering, and direct BLE communication with the main computer.
- Each reward port contains an **STM32G071-class MCU** that counts the hub-distributed 1 MHz TIMEBASE, captures inputs in hardware, and executes pre-armed outputs using timer compare.
- Reward ports communicate with the hub over a half-duplex **RS-485 linear trunk**.

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
 │                          └── differential SYNC     │
 │                                                    │
 │                                                    │
 │ RS-485 master ─────────────── data bus             │
 │ Battery protection/charger + regulated rails       │
 └────────────────────────────────────────────────────┘
              │
              │ Power, RS-485, TIMEBASE, SYNC
              │
 ┌───────────────────────────────────────────────────────┐
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

## Decisions TLDR:

- **Moving hub:** nRF52840 running Zephyr / nRF Connect SDK
- **Reward-port MCU:** Prefer STM32G071KB in a 32-pin package
- **Platform timing:** One 1 MHz differential TIMEBASE plus a separate differential SYNC line.
- **Reward-port timestamping:** 32-bit hardware counter extended to 64 bits in software.
- **Internal data bus:** Half-duplex UART over RS-485, hub-master polled, CRC protected, sequence numbered
- **Host data path:** Moving hub communicates event records directly to the computer over BLE
- **Power:** Regulated 3.3 V logic rail and a separately controlled valve/audio power domain
- **Control:** Stimuli and rewards are scheduled through hardware timer compare or pre armed local rules
- **Logging:** Log requested and actual action timestamps, command IDs, event sequence IDs, clock health records, and synchronization quality

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

# 1. Platform timing architecture

## Shared 1 MHz TIMEBASE

A stable oscillator or TCXO is divided to generate a 1 MHz differential TIMEBASE. Each reward-port MCU:

- runs its processor from an internal or local processor clock
- configures a hardware timer to count external TIMEBASE edges
- obtains one timing tick per microsecond
- does not count the timebase in an interrupt
- does not reconstruct time from RS-485 messages

Advantages:

- all reward ports share frequency directly
- processor startup and PLL behaviour do not affect event time
- distribution frequency is much lower than a 16–32 MHz processor clock
- scheduled timestamps are directly comparable between modules
- drift between reward ports is removed while the distributed clock remains healthy

## Separate SYNC line

A common timebase establishes a common rate but not a common counter value. Modules may start counting at different times. The hub therefore distributes a separate differential SYNC signal. Example semantics:

- **Session-start SYNC:** establishes the initial epoch and module offset.
- **Periodic SYNC:** captured by every module for clock-health verification.
- **Periodic SYNC should not blindly reset counters.** It should record a capture and check whether the expected offset has changed.

For module \(i\):
$H = L_i + b_i$

where:

- \(H\) is platform time
- \(L_i\) is the reward-port local external-clock counter
- \(b_i\) is the module offset measured from SYNC

Because frequency is shared, \(b_i\) should remain constant. A change indicates a fault such as:

- a missed or additional TIMEBASE edge
- a corrupted SYNC edge
- timer reconfiguration
- module reset
- cable or receiver fault

SYNC frequency is to be determined, needs can be estimated with experimental data.

## Clock-fault behaviour

Each reward port should detect:

- TIMEBASE missing
- unexpected offset change
- SYNC timeout
- timer overflow extension inconsistency
- reset during an active session

On clock failure, the reward port should:

1. force valve outputs off
2. cancel or disarm future time critical outputs
3. preserve existing event records where possible
4. create a `CLOCK_FAULT` event
5. report fault status when communication is available
6. require an explicit recovery or reinitialization sequence

## 64-bit timestamp construction

At 1 MHz, a 32-bit counter wraps after:
$2^{32}\ \mu s \approx 71.58\ \text{minutes}$

A software overflow word extends it to 64 bits:

```c
uint64_t timestamp =
    ((uint64_t)overflow_count << 32) |
    hardware_counter;
```

A 64-bit microsecond counter lasts approximately 584,000 years which should suffice.

The implementation must handle capture close to overflow without producing an incorrect high word. Exact method is to be decided on firmware.

---

# 2. Internal cable communication

## Why not I2C

I2C is not preferred for the removable multidrop platform trunk because of:

- sensitivity to bus capacitance and connector changes
- weaker noise margin near solenoid, PWM, audio, and power wiring
- awkward expansion and hot plug behaviour
- addressing limitations
- shared clock stretching and stuck bus failure modes
- reduced fault isolation

I2C remains appropriate for short, local board-level communication.

## RS-485 baseline

Using half-duplex RS-485 with the hub as the only bus master seems to be a good option.

```text
Bit rate:          500 kbit/s initial; 1 Mbit/s after validation
Electrical form:   half-duplex differential
Topology:          electrically linear daisy-chain trunk
Termination:       at the two electrical ends only
Protocol:          hub-master polling
Integrity:         CRC-16
Addressing:        assigned short address plus immutable factory UID
Recovery:          mandatory timeout and bus reset strategy
```

## Frame structure

Proposed/example logical frame:

```c
struct bus_frame_header {
    uint8_t  protocol_id;
    uint8_t  message_type;
    uint8_t  source_address;
    uint8_t  destination_address;
    uint16_t payload_length;
    uint16_t flags;
    uint32_t frame_sequence;
    uint32_t session_id;
};
```

Followed by payload and CRC. The protocol should define:

- byte order
- framing/escaping
- maximum payload
- request response timeout
- retry policy
- duplicate command behaviour
- module reset detection

## Discovery and commissioning

Each MCU exposes its factory UID. A practical discovery process:

1. hub enters commissioning mode
2. uncommissioned modules respond in pseudorandom or UID derived slots
3. hub resolves collisions through repeated rounds
4. hub assigns a short bus address
5. module identity and physical port position are recorded
6. assignment is stored with a configuration generation number

This needs to happen only when there isn't a mapping already present.

## Polling policy

Modules respond only when addressed. The hub polls:

- event count or status first
- event records in bounded batches
- health counters at a lower rate
- configuration acknowledgements after changes

---

# 3. Reward-port MCU selection and resource plan

## Selected family

Preferred baseline: **STM32G071KB** in a 32-pin package. Reasons:

- one 32-bit general purpose timer
- additional timers for capture, compare, PWM, and audio
- DMA
- multiple USARTs
- hardware CRC
- watchdogs
- 96 bit unique ID;
- more RAM and timer flexibility than the minimal G031 option.

## Pin budget

| Function                                 | Approximate GPIO count |
| ---------------------------------------- | ---------------------: |
| RS-485 TX, RX, driver-enable             |                      3 |
| TIMEBASE differential receiver output    |                      1 |
| SYNC differential receiver output        |                      1 |
| Beam-break capture                       |                      1 |
| RGB PWM                                  |                      3 |
| Audio PWM/data                           |                      1 |
| Solenoid command and optional sense      |                      2 |
| SWD debug                                |                      2 |
| Address/configuration/service            |                    1–3 |
| Status/test output                       |                      1 |
| Optional current/flow/temperature inputs |                    1–3 |

## Timer allocation - provisional

A candidate allocation is:

```text
TIM2  — 32-bit external TIMEBASE counter
TIM3  — RGB PWM channels
TIM1  — beam-break capture and/or precisely related output actions
TIM14/15/16/17 — valve, audio envelope, service timing as available
DMA   — audio waveform and event movement where useful
```

---

# 4. Peripheral event engine

## Beam-break input capture

```text
Emitter / detector
      ↓
analogue conditioning
      ↓
comparator with hysteresis
      ↓
timer input capture
      ↓
capture register or DMA
      ↓
event ring buffer
```

Hardware capture latches the timestamp at the electrical edge. Interrupt latency may delay processing but does not alter the captured value. Debouncing could use comparator hysteresis.

## Event structure

Example event record:

```c
struct port_event {
    uint64_t actual_timestamp_ticks;
    uint64_t scheduled_timestamp_ticks;
    uint32_t event_sequence;
    uint32_t command_id;
    uint32_t session_id;
    uint16_t event_type;
    uint16_t flags;
    uint32_t data;
};
```

Event types examples:

```text
BEAM_ENTER
BEAM_EXIT
CUE_ON
CUE_OFF
AUDIO_ON
AUDIO_OFF
VALVE_ON
VALVE_OFF
COMMAND_RECEIVED
COMMAND_ARMED
SYNC_CAPTURE
```

All events get captured, allows for full reconstruction of events

## Output scheduling

Use timer output compare instead of firmware delays for:

- RGB onset and offset
- audio envelope or waveform start
- valve gate on and off
- status test pulses

Command acceptance requires sufficient lead time, both for practical implementation reasons but also validation reasons.

## Conditional rules

Local rules reduce latency:

```text
IF response_window_active
AND module_is_correct_target
AND beam_event_is_valid
THEN:
    valve_on  at captured_tick + configured_delay
    valve_off at captured_tick + configured_delay + configured_duration
```

Rules should be:

- configured by the hub
- associated with a configuration generation
- bounded in capability
- validated before arming
- disabled on clock or safety fault
- logged when triggered

Exact implementation is purely on firmware level leaving it up to decision as of now.

## Command idempotency

Every command carries a unique `command_id`. Receiving the same command twice must not schedule it twice. A module should return a double command error for a duplicate command.

---

# 5. LED subsystem

Each reward port provides independently controlled RGB output.

```text
MCU timer PWM
    → current-limited LED channels
```

Requirements:

- defined off-state during reset
- configurable intensity and duration
- timer-controlled start and stop
- low-noise current paths
- no high-current LED return through sensitive detector ground
- calibrated optical onset where scientific timing requires it

---

# 6. Audio subsystem

## 8.1 Simple tones and beeps

```text
timer PWM
    → amplifier or piezo driver
    → transducer
```

## 8.2 Complex auditory stimuli

For clicks, sweeps, noise bursts, or calibrated waveforms:

```text
waveform buffer
    → timer-triggered DMA
    → high-rate PWM or DAC/codec
    → reconstruction/filter/amplifier
    → speaker
```

The MCU should trigger playback from a timer event. Interrupt driven sample generation is not acceptable for calibrated timing.

The final design depends on:

- required frequency range
- sound pressure level
- waveform complexity
- acceptable distortion
- memory requirements
- speaker and enclosure geometry

---

# 7. Solenoid and reward power

## Driver requirements

Each valve driver should include:

- logic-level MOSFET
- gate pulldown
- defined off state during reset
- flyback suppression
- local bulk capacitance
- short high current loop
- independent maximum on time protection
- watchdog behaviour that forces valve off

## Power regulation

A battery is not a stable valve rail. Valve force and timing may vary with state of charge if connected directly. Proposed architecture:

```text
Battery / protected supply
    ├── regulated 3.3 V logic domain
    └── regulated or controlled valve/audio domain
```

The design must specify:

- valve voltage and peak current
- simultaneous valve/audio activation assumptions
- cable voltage drop
- regulator transient response
- minimum battery voltage
- local capacitance
- permitted rail droop

---

# 8. Cable and physical topology

## Signal pairs

Minimum data/timing pairs:

```text
Pair 1: RS-485 A / B
Pair 2: TIMEBASE+ / TIMEBASE-
Pair 3: SYNC+ / SYNC-
```

### five pair arrangement (preferred)

```text
Pair 4: LOGIC_POWER / LOGIC_RETURN
Pair 5: VALVE_POWER / VALVE_RETURN
```

### Four pair arrangement

```text
Pair 4: distributed higher-voltage POWER / GND
```

## Module protection and test access

Each module should include:

- local decoupling
- local bulk capacitance near the valve driver
- ESD/TVS protection selected for the relevant pair
- receiver input protection
- test points for TIMEBASE, SYNC, RS-485, beam input, valve gate, logic rail, and valve rail
- controlled logic and valve return routing

---

# 11. Hub design

## Hub responsibilities

The moving nRF52840 hub owns:

- platform time authority
- trial and protocol configuration
- future timestamp command scheduling
- reward port discovery and health polling
- event collection
- sequence gap detection
- event buffering
- direct BLE telemetry to the computer
- power and battery monitoring
- fault reporting

## Buffering

BLE delivery is not real-time storage. The hub should buffer events until acknowledged or safely written to the host.

Buffer design should define:

- expected average and burst event rates
- record size
- minimum disconnected duration
- overwrite policy
- overflow behaviour

The platform should preserve timing correctness during temporary BLE interruption.

## Position tracking

?

# 10. Validation plan

## EMC validation

Test with (if tools available):

- maximum valve activity
- maximum LED PWM
- maximum audio load
- both BLE connections active
- ephys acquisition active
- complete final harness and enclosure

Record noise spectra and artefacts in the actual acquisition chain.

## Endurance validation

Running longer than the target session duration with:

- maximum expected IMU rate
- high reward port event load
- delayed or interrupted host BLE
- active timing bridge
- battery at end of discharge conditions
- worst case environmental temperature

Monitor:

- battery voltage
- regulator temperature
- buffer occupancy
- dropped frames
- BLE reconnects
- module resets
- valve rail droop
