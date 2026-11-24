# Hub synchronization system

## Purpose

This repository implements a four node timing experiment for measuring clock conversion quality, external event alignment, scheduled TTL generation, and command latency.

The system uses Hub as the reference time domain. Node and TTL generator maintain independent affine clock models that convert their local timestamps into the hub timer ticks. Adelie is a laptop client that measures application level clock exchange quality and command round trip latency through Bluetooth.

The current implementation targets nRF Connect SDK 3.4.0 for the Nordic boards and STM32 HAL with PlatformIO for node.

## Nodes

### Hub

Hub runs on an nRF52840 DK using the board target:

```text
nrf52840dk/nrf52840
```

Hub is the reference node.

Its responsibilities are:

- Run the 16 MHz reference timer
- Emit the node synchronization pulse
- Poll node over I2C
- Connect to TTL generator as a Bluetooth central
- Advertise an Adelie control service as a Bluetooth peripheral
- Build one independent affine model for node
- Build one independent affine model for TTL generator
- Capture the shared external event locally in hardware
- Convert remote event timestamps into the hub time
- Match remote events to the nearest Hub reference event
- Schedule hardware TTL pulses on TTL generator
- Capture the returned TTL generator TTL pulse on P1.12 using GPIOTE, PPI, and TIMER3
- Stream versioned CSV records over the virtual serial port

### Node

Node runs on an STM32 Nucleo G071RB.

Its responsibilities are:

- Capture the hub synchronization pulse with TIM2
- Capture the shared external event with TIM2
- Extend the local timer into a 64 bit time value
- Queue synchronization, event, and command acknowledgement records
- Return one atomic 40 byte frame per I2C read
- Receive immediate command bytes from the hub
- Report status and fault counters

Node is an I2C target at unshifted address `0x42`.

### TTL generator

TTL generator runs on an nRF54L15 DK using the board target:

```text
nrf54l15dk/nrf54l15/cpuapp
```

TTL generator advertises as:

```text
ttl_generator
```

Its responsibilities are:

- Report selected Bluetooth connection anchor timestamps
- Capture the shared external event on P1.11
- Use GPIOTE, DPPI, and GRTC for hardware event capture
- Receive scheduled TTL commands from the hub
- Generate a hardware timed TTL pulse on P1.12 using GRTC compare events, DPPI, and GPIOTE SET and CLR tasks
- Report the local timestamp associated with the generated TTL edge
- Queue outgoing 40 byte records
- Send one Bluetooth notification at a time
- Preserve a nominal 16 MHz timestamp domain by multiplying GRTC microseconds by 16

The current TTL generator report rate is approximately 1 Hz.

### Adelie

Adelie is the laptop client.

Its responsibilities are:

- Connect to the hub Adelie service
- Perform repeated application clock exchanges
- Fit a rolling 16 point Adelie to the hub clock model
- Send immediate node commands through Hub
- Record command stage notifications
- Save clock and latency CSV files
- Produce host side summaries

## Main timing domains

Hub uses a 16 MHz timer as the common reference domain.

Node reports local TIM2 ticks at 16 MHz.

TTL generator receives GRTC timestamps at 1 MHz and multiplies them by 16 before transmission. Bluetooth anchor timestamps and scheduled TTL timestamps are represented in the same nominal 16 MHz domain.

Adelie uses host monotonic nanoseconds at 1 GHz.

The stream identifies each timestamp frequency explicitly for normal event rows.

## Clock model

Each embedded remote node has an independent 16 point affine model.

The reference point form reduces numerical loss when timestamps become large.

The model has two operating states:

- `ACQUIRE` means there are not yet enough accepted points or the model was reset
- `TRACK` means the current model passed admission checks and can convert timestamps

A model reset begins a new analysis segment. Fits and interval calculations must not cross a reset boundary.

## Hardware synchronization paths

### Node synchronization path

Hub emits a periodic pulse using TIMER2, PPI, and GPIOTE.

The nominal current rate is 4 Hz.

The pulse path is:

```text
Hub TIMER2 compare
→ PPI
→ GPIOTE output task
→ node TIM2 input capture
```

Hub associates a returned node synchronization record with its own pulse schedule. Node does not need to assign the hub pulse number.

### TTL generator synchronization path

Hub and TTL generator both observe Bluetooth connection event anchors from their controllers.

TTL generator sends selected anchor reports to the hub in the shared 40 byte frame. Hub matches the Bluetooth event counter and maps the controller anchor into the hub timer domain.

TTL generator currently sends approximately one selected anchor report per second.

### Scheduled TTL generator TTL path

Hub periodically chooses a target time in the hub domain and uses the current TTL generator affine model to calculate the corresponding TTL generator local target.

Hub sends a 20 byte scheduling command over the TTL generator TTL control characteristic. TTL generator uses two absolute GRTC compare events to generate the rising and falling edges entirely in hardware.

The output path is:

```text
TTL generator GRTC SET compare
→ DPPI connection
→ GPIOTE SET task
→ TTL generator P1.12 rising edge
→ Hub P1.12 input
→ GPIOTE event
→ PPI connection
→ Hub TIMER3 capture
```

A second GRTC compare triggers the TTL generator GPIOTE CLR task at the requested pulse width.

Hub TIMER3 is cleared from the same TIMER2 compare event that begins each 4 Hz reference interval. This keeps TIMER3 phase aligned with TIMER2 while leaving the existing TIMER2 capture channels unchanged.

The standard automated test currently uses:

```text
lead time 300 ms
period 1000 ms
pulse width 100 us
timeout 250 ms after the target
```

Hub logs the scheduled target, the generated timestamp reported by TTL generator, the locally acquired edge, and the combined result.

## External event capture

A shared rising edge should be wired to the event input on each participating embedded node.

Hub uses hardware capture into TIMER2.

Node uses TIM2 input capture.

TTL generator uses:

```text
P1.11 rising edge
→ GPIOTE event
→ DPPI connection
→ GRTC capture task
→ GRTC compare register
```

The TTL generator interrupt handler reads the already captured register value. Interrupt latency is not part of the event timestamp.

The TTL generator overlay assigns the Zephyr user event GPIO property to GPIO controller 1 pin 11 with active high polarity.

## Suggested wiring

### Hub and node

- Hub I2C SCL on P0.27 to node PB8
- Hub I2C SDA on P0.26 to node PB9
- Hub synchronization output on P1.10 to node PA0
- Shared external event to the hub P1.11
- Shared external event to node PA1
- Common ground between boards

Use one suitable pair of I2C pull resistors to the correct application voltage.

Do not connect independently powered USB board power rails together.

### TTL generator event input

- Shared external event to TTL generator P1.11
- Common ground with the event generator and the other boards

Verify that the event source uses safe logic levels for every connected board.

### TTL generator scheduled TTL output

- TTL generator P1.12 to the hub P1.12
- TTL generator ground to the hub ground
- Keep the P1.11 shared external event wiring separate from the P1.12 scheduled TTL wiring

The current direct digital connection assumes that TTL generator produces a 3.3 V compatible high level for the hub. Do not connect the two board power rails together when both boards are independently USB powered.

## Repository layout

```text
hub/
    src/
        main.c
        command_protocol.h
    boards/
        nrf52840dk_nrf52840.overlay
    prj.conf

node/
    src/
        main.c
    platformio.ini

ttl_generator/
    src/
        main.c
    boards/
        nrf54l15dk_nrf54l15_cpuapp.overlay
    prj.conf

adelie/
    adelie.py
    requirements.txt

analysis/
    parse_sync_log.py
    analyse_sync.py
    plot_sync.py
    run_all.py

README.md
PROTOCOL.md
```

Repository also includes `arduino_pulse` which is the GPIO event generator made from an `arduino due` on PlatformIO.

## Bring up order

1. Flash node and connect the I2C wiring

2. Flash TTL generator and confirm that it advertises

3. Flash Hub

4. Open Hub VCOM0 at 115200 baud using 8 data bits, no parity, and 1 stop bit

5. Confirm that Hub prints `SCHEMA,3`

6. Confirm that Hub prints its `READY` record and `TTL_INPUT_READY`

7. Confirm that the node link produces `PAIR_RAW` and `SYNC` records

8. Confirm that the TTL generator link reaches connected and subscribed states

9. Wait for both remote clock models to reach `TRACK`

10. Confirm that the scheduled TTL test produces `TTL_PULSE_SCHEDULED`, `TTL_PULSE_GENERATED`, `TTL_PULSE_ACQUIRED`, and `TTL_PULSE_RESULT`

11. Start Adelie and confirm clock replies and command stage notifications

12. Apply a shared GPIO pulse and confirm Hub, node, and TTL generator event records

13. Record the hub serial stream for analysis

## Expected startup records

A healthy Hub stream begins with:

```text
SCHEMA,3
```

It then prints schema comments and a `READY` row.

The Hub TTL input should report:

```text
TTL_INPUT_READY,hub,pin=44,timer=3,cc=0
TTL_TEST_CONFIG,lead_ms=300,period_ms=1000,width_us=100,ttl_input_pin=44
```

Typical link activity includes:

```text
LINK,ttl_generator,BLE,CENTRAL,SCANNING,ttl_generator,0,0
LINK,ttl_generator,BLE,CENTRAL,CONNECTED,ttl_generator,10000,0
LINK,ttl_generator,BLE,CENTRAL,SUBSCRIBED,ttl_generator,10000,0
```

Exact reason and interval values depend on the current firmware.

TTL generator should report:

```text
EVENT_CAPTURE_READY,ttl_generator,pin=43,gpiote_ch=0,grtc_ch=0
TTL_OUTPUT_READY,ttl_generator,pin=44,gpiote_ch=1,set_grtc_ch=1,clear_grtc_ch=2
BLE_ADVERTISING,ttl_generator
READY,ttl_generator,1,40,HARDWARE_CAPTURE,event_pin=43,ttl_output_pin=44,grtc_ch=0
```

Channel values are allocated at runtime and can differ.

## Serial recording

Hub normally appears as VCOM0 through the debugger interface.

Use:

```text
115200 baud
8 data bits
no parity
1 stop bit
no software flow control
no hardware flow control
```

Python script `serial_record.py` does a good job of that.

## Analysis workflow

The analysis pipeline has three main stages.

### Parse

`parse_sync_log.py` reads the hub serial stream and the Adelie CSV files.

It produces normalized CSV files for pairs, synchronization rows, events, matches, diagnostics, links, faults, scheduled TTL records, and Adelie measurements.

### Analyse

`analyse_sync.py` derives:

- Adjacent interval clock error in ppm
- Firmware slope error in ppm
- Firmware RMS in microseconds
- Independent rolling fit RMS
- Model resets and rejected fits
- Event signed error
- Event absolute error
- Event transport age
- Event matching counts
- Scheduled TTL generation error
- Scheduled TTL acquisition error
- TTL generator generated to the hub acquired offset
- Scheduled TTL completion and timeout counts
- Adelie clock exchange RTT
- Adelie rolling model quality
- Command stage latency
- End to end command RTT

The node summaries and scheduled TTL path are written into one integrated `summary.csv` and `summary.txt`.

### Plot

`plot_sync.py` produces selected diagnostic plots.

Useful plot groups include:

- Clock rate variation
- Firmware and rolling RMS
- Model residuals
- External event error
- Event error distribution
- Event error compared with transport age
- Scheduled TTL error components
- Scheduled TTL total error distribution
- Scheduled TTL success and timeout outcomes
- Adelie network RTT
- Adelie model prediction error
- Command latency breakdown

## Interpretation notes

A positive adjacent interval error means the remote clock accumulated more nominal time than the hub during that interval.

The firmware slope often has the opposite sign because it maps the faster remote clock back into the hub domain.

Firmware RMS describes scatter of the points inside the active model window.

One step prediction error evaluates how well the previous model predicts the next point.

Event signed error can have a median near zero even when the absolute error is large. Positive and negative errors cancel in the signed statistic.

Transport age is not the synchronization error. It describes how old the captured event was when it was reported.

For the scheduled TTL path:

```text
generation error = converted TTL generator generated time minus target Hub time
wire offset = Hub acquired time minus converted TTL generator generated time
total error = Hub acquired time minus target Hub time
```

`total_error_ns` is the main end to end result. `wire_offset_ns` contains fixed hardware path delay, signal threshold delay, timer reconstruction error, and remaining TTL generator to the hub clock model error. A negative value does not mean that electrical propagation occurred backwards in time.

Adelie one way latency values are estimates because they depend on a noisy host to the hub clock model and on path symmetry assumptions. End to end command RTT is the strongest Adelie latency measurement.
