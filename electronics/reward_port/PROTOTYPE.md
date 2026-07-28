# Nucleo master connection table

| Function                   | STM32 pin | Label printed beside Arduino socket | Physical Nucleo connection |
| -------------------------- | --------- | ----------------------------------- | -------------------------- |
| SYNC capture               | PA0       | A0                                  | CN8 pin 1                  |
| Light-gate digital capture | PA1       | A1                                  | CN8 pin 2                  |
| Audio DAC                  | PA4       | A2                                  | CN8 pin 3                  |
| Light-gate ADC             | PA6       | D12                                 | CN5 pin 5                  |
| Valve PWM                  | PA8       | D7                                  | CN9 pin 8                  |
| RS-485 TX                  | PA9       | D8                                  | CN5 pin 1                  |
| RS-485 RX                  | PA10      | D2                                  | CN9 pin 3                  |
| Optional RS-485 DE         | PA12      | —                                   | CN10 pin 12                |
| RGB channel 3 / blue       | PB0       | D10                                 | CN5 pin 3                  |
| RGB channel 1 / red        | PB4       | D5                                  | CN9 pin 6                  |
| RGB channel 2 / green      | PB5       | D4                                  | CN9 pin 5                  |
| Amplifier shutdown         | PC0       | —                                   | CN10 pin 18                |
| IR emitter enable          | PC1       | —                                   | CN10 pin 16                |
| 3.3 V                      | —         | 3V3                                 | CN6 pin 4                  |
| 5 V                        | —         | 5V                                  | CN6 pin 5                  |
| Ground                     | —         | GND                                 | CN6 pin 6 or 7             |

# Overall power arrangement

| Supply                         | Use                                 |
| ------------------------------ | ----------------------------------- |
| Nucleo USB                     | MCU, programming and serial logging |
| Nucleo 3V3                     | Light gate and Grove RS-485 logic   |
| Nucleo 5V                      | RGB breakout during initial testing |
| External current-limited VLOAD | DRV8837EVM and valve                |
| External regulated 5V          | Preferably the audio amplifier      |

---

## 1. Direct SYNC test

For the first test, do not make SYNC differential. Connect the hub GPIO directly:

| From                 | Through               | To              |
| -------------------- | --------------------- | --------------- |
| Hub SYNC output GPIO | 120 Ω series resistor | Nucleo A0 / PA0 |
| Hub GND              | Direct                | Nucleo GND      |

The 120 Ω here is acting only as a small protective series resistor; it is **not bus termination**.

Configure the hub pin as a push-pull 3.3 V output and PA0 as TIM2_CH1 input capture.

---

## 1. Omron EE-SX1140 light gate

### Optical switch pin wiring

Identify the four Omron terminals using the `A`, `K`, `C`, and `E` markings:

| EE-SX1140 terminal | Meaning                   | Connect to                 |
| ------------------ | ------------------------- | -------------------------- |
| A                  | IR LED anode              | 3V3 through 100 Ω          |
| K                  | IR LED cathode            | Drain of IR-control 2N7000 |
| C                  | Phototransistor collector | BEAM_SENSE node            |
| E                  | Phototransistor emitter   | GND                        |

### BEAM_SENSE circuit

Wire the detector side like this:

```text
                  3V3
                   |
                  10k
                   |
                   +--------- PA6 / D12 / ADC
                   |
BEAM_SENSE --------+
                   |
                   +--- 1k --- PA1 / A1 / TIM2_CH2
                   |
                  470pF
                   |
                  GND

EE-SX1140 collector C -> BEAM_SENSE
EE-SX1140 emitter E   -> GND
```

PA6 gets the analogue measurement directly. PA1 receives the same node through 1 kΩ for timer capture.

## IR emitter MOSFET

Use one of the four 2N7000s:

| Connection          | Wire to                                           |
| ------------------- | ------------------------------------------------- |
| 2N7000 source       | GND                                               |
| 2N7000 gate         | PC1 / CN10 pin 16 through approximately 100–120 Ω |
| Gate pull-down      | 100 kΩ from gate to GND                           |
| 2N7000 drain        | EE-SX1140 cathode `K`                             |
| EE-SX1140 anode `A` | 3V3 through 100 Ω                                 |

## 3. DFRobot RGB breakout

### Breakout connections

| DFR0239 pin         | Connect to            |
| ------------------- | --------------------- |
| `5V` / common anode | Nucleo 5V             |
| `R`                 | Drain of red MOSFET   |
| `G`                 | Drain of green MOSFET |
| `B`                 | Drain of blue MOSFET  |

### MOSFET connections

| Colour            | MOSFET gate | Nucleo socket |
| ----------------- | ----------- | ------------- |
| Red / channel 1   | PB4         | D5            |
| Green / channel 2 | PB5         | D4            |
| Blue / channel 3  | PB0         | D10           |

For each MOSFET:

```text
Nucleo PWM ---- 100 to 120 ohms ---- Gate
                                      |
                                    100k
                                      |
                                     GND

MOSFET source -> GND
MOSFET drain  -> corresponding R, G or B module pin
```

Because the breakout already contains channel resistors, do **not** add another set of 330 Ω resistors unless the LED is too bright.

## 4. DRV8837EVM and valve

### EVM wiring

| DRV8837EVM connection | Connect to                            |
| --------------------- | ------------------------------------- |
| J1 `VM`               | External current-limited VLOAD        |
| J1 `GND`              | External supply GND and Nucleo GND    |
| TP1 `IN1`             | Nucleo PA8 / D7                       |
| TP2 `IN2`             | GND                                   |
| J4 `OUT1`             | Valve terminal 1                      |
| J4 `OUT2`             | Valve terminal 2                      |
| JP2                   | Installed, driver active              |
| EVM USB               | Leave disconnected when J1 is powered |

With this wiring:

| PA8 / IN1 | IN2 | Result                  |
| --------: | --: | ----------------------- |
|         0 |   0 | Valve outputs coast/off |
|         1 |   0 | Valve driven            |
|       PWM |   0 | Drive/coast PWM         |

## 5. Grove RS-485 board

Power the Grove module from **3.3 V**, rather than 5 V, so its UART-side logic remains at a Nucleo-compatible level.

| Grove RS-485 signal       | Connect to Nucleo      |
| ------------------------- | ---------------------- |
| Module UART input / `RX`  | PA9 / D8 / USART1_TX   |
| Module UART output / `TX` | PA10 / D2 / USART1_RX  |
| VCC                       | 3V3                    |
| GND                       | GND                    |
| A                         | RS-485 A on hub module |
| B                         | RS-485 B on hub module |

The Grove board has automatic transmit-direction control, so **PA12/DE is not connected for the prototype**. Configure USART1 for 500,000 baud, 8-N-1, normal UART mode rather than STM32 hardware-DE mode.

Across the cable:

```text
Hub module A -------- Reward-port module A
Hub module B -------- Reward-port module B
Hub ground  --------- Reward-port ground
```

## 6. PAM8302A audio prototype

With the notch or pin-1 dot used to orient the chip:

| PAM8302A pin | Function            | Prototype connection                                    |
| -----------: | ------------------- | ------------------------------------------------------- |
|            1 | Active-low shutdown | PC0 / CN10 pin 18                                       |
|            2 | NC                  | Leave unconnected                                       |
|            3 | IN+                 | DAC signal through filter and 0.1 µF coupling capacitor |
|            4 | IN−                 | 0.1 µF capacitor to GND                                 |
|            5 | VO+                 | Speaker terminal 1                                      |
|            6 | VDD                 | Regulated 5 V                                           |
|            7 | GND                 | Common GND                                              |
|            8 | VO−                 | Speaker terminal 2                                      |

## DAC-to-amplifier input

```text
PA4 / A2
   |
   1k
   |
   +---------- 0.1uF series capacitor ---------- PAM pin 3 IN+
   |
  4.7nF
   |
  GND

PAM pin 4 IN- -------- 0.1uF -------- GND
```

## Power and shutdown

```text
5V -------- PAM pin 6
 |
 +--- 1uF --- GND
 |
 +--- 10uF -- GND

PAM pin 7 -------- GND

PC0 / CN10 pin 18 -------- PAM pin 1 SD
                              |
                            100k
                              |
                             GND
```

The pull-down keeps the amplifier shut down while the MCU is resetting. Set PC0 high to enable audio.

```text
PAM pin 5 VO+ ---- speaker ---- PAM pin 8 VO-
```
