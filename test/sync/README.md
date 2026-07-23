# Korora synchronization system

## Purpose

This repository implements a four node timing experiment for measuring clock conversion quality, external event alignment, and command latency.

The system uses Korora as the reference time domain. Fairy and Galapagos maintain independent affine clock models that convert their local timestamps into Korora timer ticks. Adelie is a laptop client that measures application level clock exchange quality and command round trip latency through Bluetooth.

The current implementation targets nRF Connect SDK 3.4.0 for the Nordic boards and STM32 HAL with PlatformIO for Fairy.

## Nodes

### Korora

Korora runs on an nRF52840 DK using the board target:

```text
nrf52840dk/nrf52840
```

Korora is the reference node.

Its responsibilities are:

- Run the 16 MHz reference timer
- Emit the Fairy synchronization pulse
- Poll Fairy over I2C
- Connect to Galapagos as a Bluetooth central
- Advertise an Adelie control service as a Bluetooth peripheral
- Build one independent affine model for Fairy
- Build one independent affine model for Galapagos
- Capture the shared external event locally in hardware
- Convert remote event timestamps into Korora time
- Match remote events to the nearest Korora reference event
- Stream versioned CSV records over the virtual serial port

### Fairy

Fairy runs on an STM32 Nucleo G071RB.

Its responsibilities are:

- Capture the Korora synchronization pulse with TIM2
- Capture the shared external event with TIM2
- Extend the local timer into a 64 bit time value
- Queue synchronization, event, and command acknowledgement records
- Return one atomic 40 byte frame per I2C read
- Receive immediate command bytes from Korora
- Report status and fault counters

Fairy is an I2C target at unshifted address `0x42`.

### Galapagos

Galapagos runs on an nRF54L15 DK using the board target:

```text
nrf54l15dk/nrf54l15/cpuapp
```

Galapagos advertises as:

```text
galapagos
```

Its responsibilities are:

- Report selected Bluetooth connection anchor timestamps
- Capture the shared external event on P1.11
- Use GPIOTE, DPPI, and GRTC for hardware event capture
- Queue outgoing 40 byte records
- Send one Bluetooth notification at a time
- Preserve a nominal 16 MHz timestamp domain by multiplying GRTC microseconds by 16

The current Galapagos report rate is approximately 1 Hz.

### Adelie

Adelie is the laptop client.

Its responsibilities are:

- Connect to the Korora Adelie service
- Perform repeated application clock exchanges
- Fit a rolling 16 point Adelie to Korora clock model
- Send immediate Fairy commands through Korora
- Record command stage notifications
- Save clock and latency CSV files
- Produce host side summaries

## Main timing domains

Korora uses a 16 MHz timer as the common reference domain.

Fairy reports local TIM2 ticks at 16 MHz.

Galapagos receives GRTC timestamps at 1 MHz and multiplies them by 16 before transmission. Bluetooth anchor timestamps are also represented in the same nominal 16 MHz domain.

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

### Fairy synchronization path

Korora emits a periodic pulse using TIMER2, PPI, and GPIOTE.

The nominal current rate is 4 Hz.

The pulse path is:

```text
Korora TIMER2 compare
→ PPI
→ GPIOTE output task
→ Fairy TIM2 input capture
```

Korora associates a returned Fairy synchronization record with its own pulse schedule. Fairy does not need to assign the Korora pulse number.

### Galapagos synchronization path

Korora and Galapagos both observe Bluetooth connection event anchors from their controllers.

Galapagos sends selected anchor reports to Korora in the shared 40 byte frame. Korora matches the Bluetooth event counter and maps the controller anchor into the Korora timer domain.

Galapagos currently sends approximately one selected anchor report per second.

## External event capture

A shared rising edge should be wired to the event input on each participating embedded node.

Korora uses hardware capture into TIMER2.

Fairy uses TIM2 input capture.

Galapagos uses:

```text
P1.11 rising edge
→ GPIOTE event
→ DPPI connection
→ GRTC capture task
→ GRTC compare register
```

The Galapagos interrupt handler reads the already captured register value. Interrupt latency is not part of the event timestamp.

The Galapagos overlay assigns the Zephyr user event GPIO property to GPIO controller 1 pin 11 with active high polarity.

## Suggested wiring

### Korora and Fairy

- Korora I2C SCL on P0.27 to Fairy PB8
- Korora I2C SDA on P0.26 to Fairy PB9
- Korora synchronization output on P1.10 to Fairy PA0
- Shared external event to Korora P1.11
- Shared external event to Fairy PA1
- Common ground between boards

Use one suitable pair of I2C pull resistors to the correct application voltage.

Do not connect independently powered USB board power rails together.

### Galapagos event input

- Shared external event to Galapagos P1.11
- Common ground with the event generator and the other boards

Verify that the event source uses safe logic levels for every connected board.

## Repository layout

```text
korora/
    src/
        main.c
        adelie_protocol.h
    boards/
        nrf52840dk_nrf52840.overlay
    prj.conf

fairy/
    src/
        main.c
    platformio.ini

galapagos/
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

1. Flash Fairy and connect the I2C wiring

2. Flash Galapagos and confirm that it advertises

3. Flash Korora

4. Open Korora VCOM0 at 115200 baud using 8 data bits, no parity, and 1 stop bit

5. Confirm that Korora prints `SCHEMA,3`

6. Confirm that Korora prints its `READY` record

7. Confirm that the Fairy link produces `PAIR_RAW` and `SYNC` records

8. Confirm that the Galapagos link reaches connected and subscribed states

9. Wait for both remote clock models to reach `TRACK`

10. Start Adelie and confirm clock replies and command stage notifications

11. Apply a shared GPIO pulse and confirm Korora, Fairy, and Galapagos event records

12. Record the Korora serial stream for analysis

## Expected startup records

A healthy Korora stream begins with:

```text
SCHEMA,3
```

It then prints schema comments and a `READY` row.

Typical link activity includes:

```text
LINK,galapagos,BLE,CENTRAL,SCANNING,galapagos,0,0
LINK,galapagos,BLE,CENTRAL,CONNECTED,galapagos,10000,0
LINK,galapagos,BLE,CENTRAL,SUBSCRIBED,galapagos,10000,0
```

Exact reason and interval values depend on the current firmware.

Galapagos should report:

```text
EVENT_CAPTURE_READY,galapagos,pin=43,gpiote_ch=0,grtc_ch=0
BLE_ADVERTISING,galapagos
READY,galapagos,1,40,HARDWARE_CAPTURE,grtc_ch=0
```

Channel values are allocated at runtime and can differ.

## Serial recording

Korora normally appears as VCOM0 through the debugger interface.

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

`parse_sync_log.py` reads the Korora serial stream and the Adelie CSV files.

It produces normalized CSV files for pairs, synchronization rows, events, matches, diagnostics, links, faults, and Adelie measurements.

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
- Adelie clock exchange RTT
- Adelie rolling model quality
- Command stage latency
- End to end command RTT

### Plot

`plot_sync.py` produces selected diagnostic plots.

Useful plot groups include:

- Clock rate variation
- Firmware and rolling RMS
- Model residuals
- External event error
- Event error distribution
- Event error compared with transport age
- Adelie network RTT
- Adelie model prediction error
- Command latency breakdown

## Interpretation notes

A positive adjacent interval error means the remote clock accumulated more nominal time than Korora during that interval.

The firmware slope often has the opposite sign because it maps the faster remote clock back into the Korora domain.

Firmware RMS describes scatter of the points inside the active model window.

One step prediction error evaluates how well the previous model predicts the next point.

Event signed error can have a median near zero even when the absolute error is large. Positive and negative errors cancel in the signed statistic.

Transport age is not the synchronization error. It describes how old the captured event was when it was reported.

Adelie one way latency values are estimates because they depend on a noisy host to Korora clock model and on path symmetry assumptions. End to end command RTT is the strongest Adelie latency measurement.
