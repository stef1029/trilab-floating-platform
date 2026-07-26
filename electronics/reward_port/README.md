# Reward Port

Standalone STM32G071 based reward port controller for a behavioural rig. Each reward port provides:

- Differential RS-485 communication with the hub
- Differential SYNC input with hardware timestamp capture
- Omron EE-SX1140 light gate capture
- RGB visual cue output
- Band limited noise playback
- Lee LHDB0342115H solenoidvalve control
- Local event buffering and clock translation

The final current PCB uses an STM32G071RBT6 due to the wired prototype which uses a NUCLEO-G071RB so that the same MCU peripherals and firmware can be tested before manufacturing the PCB.

## System architecture

The nRF52840 hub is the system time authority and RS-485 bus master. The reward port has its own local timer. There is no external oscillator, distributed TIMEBASE, or TCXO.

The hub periodically emits a differential SYNC pulse, initially at 4 Hz. Each reward port captures the pulse using TIM2 hardware input capture. The reward port reports its captured local timestamp to the hub.

## Power rails

The reward port receives:

- 3V3 for the STM32 and communication transceivers
- VLOAD for the solenoid, audio amplifier, and RGB LED (direct battery supply, 5V from devboard)
- Ground

## Final MCU pin assignment

| Function                        | GPIO | LQFP64 pin | Peripheral  |
| ------------------------------- | ---: | ---------: | ----------- |
| Differential SYNC capture       |  PA0 |         17 | TIM2_CH1    |
| Light-gate capture              |  PA1 |         18 | TIM2_CH2    |
| Audio waveform                  |  PA4 |         21 | DAC1_OUT1   |
| Light-gate analogue measurement |  PA6 |         23 | ADC_IN6     |
| Solenoid PWM                    |  PA8 |         36 | TIM1_CH1    |
| RS-485 transmit                 |  PA9 |         37 | USART1_TX   |
| RS-485 receive                  | PA10 |         42 | USART1_RX   |
| RS-485 driver enable            | PA12 |         44 | USART1_DE   |
| SWD data                        | PA13 |         45 | SWDIO       |
| SWD clock                       | PA14 |         46 | SWCLK       |
| RGB channel 3                   |  PB0 |         27 | TIM3_CH3    |
| RGB channel 1                   |  PB4 |         58 | TIM3_CH1    |
| RGB channel 2                   |  PB5 |         59 | TIM3_CH2    |
| Audio amplifier shutdown        |  PC0 |          8 | GPIO output |
| IR emitter enable               |  PC1 |          9 | GPIO output |
| Reset                           | NRST |          7 | NRST        |

The physical RGB colours depend on the final cable wiring. Firmware should name the outputs by physical channel until the harness is fixed.

## Daisy-chain connector

The final PCB uses two identical 10-pin JST GH connectors:

- J2 IN
- J3 OUT

They are connected pin for pin.

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

Pins 7 and 9 are the same VLOAD net. Pins 6, 8, and 10 join the same PCB ground plane. Only the two physical ends of each differential bus are terminated.

The final end of line plug contains:

- 120 ohms across RS485_P and RS485_N
- 120 ohms across SYNC_P and SYNC_N

Intermediate reward ports do not install termination.

## RS-485 firmware

Initial configuration:

- USART1
- 500000 baud
- 8 data bits
- No parity
- 1 stop bit
- Half duplex
- Hardware driver enable output on PA12
- Hub master polling
- Reward ports transmit only when addressed

PA12 must default low so the reward port cannot drive the bus while resetting.

## SYNC capture

PA0 is configured as TIM2_CH1 input capture.

Recommended timer configuration:

- TIM2 timer clock: 64 MHz
- Prescaler: 63
- Counter rate: 1 MHz
- Counter period: 0xFFFFFFFF
- Counter mode: up
- CH1 capture: rising edge
- CH2 capture: rising edge

## Omron EE-SX1140 light gate

The light gate is an Omron EE-SX1140 slotted optical switch with:

- IR LED emitter
- Phototransistor detector
- 14 mm slot

Final connector wiring:

| J5 pin | Signal                                   |
| -----: | ---------------------------------------- |
|      1 | Phototransistor collector / BEAM_SENSE   |
|      2 | Phototransistor emitter / GND            |
|      3 | IR LED cathode / switched by MOSFET      |
|      4 | IR LED anode / supplied through 120 ohms |

Detector circuit:

    3V3
      |
     10k
      |
      +------ BEAM_SENSE ------ PA6 / ADC_IN6
      |
      +------ 1k ------ PA1 / TIM2_CH2
      |
     470pF
      |
     GND

    phototransistor collector -> BEAM_SENSE
    phototransistor emitter   -> GND

Signal behaviour:

- Clear slot: phototransistor conducts and BEAM_SENSE is low
- Blocked slot: phototransistor stops conducting and BEAM_SENSE rises
- TIM2_CH2 rising capture therefore records beam interruption

No external comparator is used. PA6 measures BEAM_SENSE with the ADC for setup and diagnostics. It must remain configured as an analogue input. PA1 must be configured as TIM2_CH2 alternate function input. At startup, record the ADC level with the slot clear and blocked. Generate a sensor fault when the two states do not provide a safe digital margin.

### IR emitter control

PC1 controls the low side IR emitter MOSFET.

At startup:

1. Keep the emitter disabled
2. Measure the dark detector level
3. Enable the emitter
4. Measure the clear-slot level
5. Verify that the expected transition occurred
6. Report a sensor fault when the test fails

The emitter may remain enabled during an active trial.

## RGB output

TIM3 produces three independent PWM outputs:

- PB4 / TIM3_CH1
- PB5 / TIM3_CH2
- PB0 / TIM3_CH3

Initial PWM frequency:

    2000 Hz

The remote LED is common anode:

- Common anode to VLOAD
- Each cathode to its resistor and low-side MOSFET

Firmware must guarantee that all RGB channels are off during reset and before configuration is received.

Commands should support:

- RGB channel values
- Scheduled start timestamp
- Scheduled stop timestamp
- Command ID
- Session ID

## Audio

PA4 / DAC1_OUT1 generates the audio waveform. The final PCB uses a PAM8302A mono Class-D amplifier. The prototype may use a different analogue input Class-D breakout while validating the DAC, waveform generation, speaker, and enclosure.

Recommended initial implementation:

- TIM6 triggers DAC1
- DAC1 uses DMA
- Sample rate: 96000 samples per second
- Double buffered waveform generation
- PRNG noise source
- Configurable digital band-pass filter
- Configurable amplitude
- 5 to 10 ms onset and offset ramps

Initial test band:

    8000 Hz to 16000 Hz

This range is provisional. The actual usable range and level must be measured with the final speaker and enclosure at the animal position. PC0 controls amplifier shutdown. The speaker is connected between amplifier OUT+ and OUT-. Neither speaker wire is ground.

## Solenoid valve

Valve:

    Lee Company LHDB0342115H

Driver:

    ROHM BD6210F-E2

Connections:

- BD6210F OUT1 to valve terminal 1
- BD6210F OUT2 to valve terminal 2
- BD6210F VCC pins to VLOAD
- BD6210F VREF to VLOAD
- BD6210F RIN to GND
- BD6210F FIN to PA8 / TIM1_CH1
- BD6210F GND to common ground

The valve is driven between OUT1 and OUT2. It is not connected between one output and ground.

Place the 100 nF, 10 uF, and large bulk capacitor directly across VLOAD and ground near the BD6210F.

Initial valve PWM:

    25000 Hz

Initial drive sequence:

1. Apply spike PWM
2. Maintain the spike for approximately 10 to 15 ms
3. Change to hold PWM
4. Hold for the commanded reward duration
5. Set FIN low
6. Record actual valve-off time

The Lee valve specifies approximately:

- 3.3 V actuation spike
- 2.0 V hold voltage
- 310 mW hold power

Initial duty profiles must be calibrated with the actual supply and valve.

Firmware must include:

- Absolute maximum valve on timeout
- Valve off during reset
- Valve off on watchdog reset
- Valve off after communication/session fault
- Rejection of overlapping valve commands
- Minimum interval between valve activations
- Logged requested and actual valve times

## Clock and startup configuration

Use:

- Internal HSI16 oscillator
- PLL system clock at 64 MHz
- No external crystal
- No external TIMEBASE
- Brownout reset enabled
- Independent watchdog enabled
- SWD enabled on PA13 and PA14

Startup order:

1. Force valve, RGB, IR, and amplifier outputs into safe states
2. Initialize clocks
3. Initialize TIM2 and its overflow extension
4. Initialize event buffers
5. Initialize ADC and perform light-gate self-test
6. Initialize USART1 and RS-485 driver enable
7. Wait for hub discovery and session configuration
8. Accept commands only after synchronization is valid

## Prototype bring-up order

Bring up one subsystem at a time:

1. Nucleo serial logging
2. TIM2 free-running 1 MHz timer
3. Single-ended test pulse into PA0
4. Differential SYNC through two RS-485 breakouts
5. EE-SX1140 capture and ADC measurement
6. RGB PWM
7. DAC waveform generation
8. Audio amplifier and speaker
9. BD6210F without valve attached
10. BD6210F with valve and current-limited VLOAD supply
11. RS-485 protocol
12. Full clock translation and event logging
