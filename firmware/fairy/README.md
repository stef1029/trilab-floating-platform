# Fairy firmware

Fairy runs on STM32G071. It supports the NUCLEO G071RB prototype and the planned STM32G071GBU6 board through one board profile file.

## Responsibilities

- Read the 96 bit STM32 factory UUID
- Join the RS485 bus through Magellan
- Capture 4 Hz SYNC on TIM2 channel 1
- Capture light gate interruption on TIM2 channel 2
- Debounce light gate edges for 100 ms
- Buffer Fairy v3 records until Korora acknowledges them
- Drive RGB at 2 kHz
- Generate tone or band limited white noise with DAC DMA
- Apply a 10 ms audio onset and offset ramp
- Drive the valve at 25 kHz with spike and hold duty
- Enforce valve safety limits
- Run an independent watchdog
- Perform a light gate startup self test
- Stop the local session after two seconds without Korora contact

## Select a profile

`FAIRY_BOARD_PROFILE=1` selects the Nucleo prototype.

`FAIRY_BOARD_PROFILE=2` selects the final package pin plan.

All pin decisions are in:

```text
include/board_profile.hpp
src/board_profile.cpp
```

This is the intended place to change a prototype pin.

## Prototype pins

The user supplied PC4 and PC5 choice is used for USART1. The automatic
direction Grove module means no direction pin is driven.

| Function           | STM32 pin   | Peripheral     |
| ------------------ | ----------- | -------------- |
| SYNC capture       | PA0         | TIM2 channel 1 |
| Light gate capture | PA1         | TIM2 channel 2 |
| Audio DAC          | PA4         | DAC1 output 1  |
| Light gate ADC     | PA6         | ADC input 6    |
| Valve PWM          | PA8         | TIM1 channel 1 |
| RS485 TX           | PC4         | USART1 TX      |
| RS485 RX           | PC5         | USART1 RX      |
| RGB blue           | PB0         | TIM3 channel 3 |
| RGB red            | PB4         | TIM3 channel 1 |
| RGB green          | PB5         | TIM3 channel 2 |
| Amplifier shutdown | PC0         | GPIO           |
| IR emitter enable  | PC1         | GPIO           |
| Human debug UART   | PA2 and PA3 | USART2         |

## Final PCB pins

| Function           | STM32 pin | Peripheral     |
| ------------------ | --------- | -------------- |
| SYNC capture       | PA0       | TIM2 channel 1 |
| Light gate capture | PA1       | TIM2 channel 2 |
| Amplifier shutdown | PA2       | GPIO           |
| IR emitter enable  | PA3       | GPIO           |
| Audio DAC          | PA4       | DAC1 output 1  |
| Light gate ADC     | PA6       | ADC input 6    |
| RGB blue           | PB0       | TIM3 channel 3 |
| Valve PWM          | PA8       | TIM1 channel 1 |
| RS485 direction    | PB3       | GPIO           |
| RGB red            | PB4       | TIM3 channel 1 |
| RGB green          | PB5       | TIM3 channel 2 |
| RS485 TX           | PB6       | USART1 TX      |
| RS485 RX           | PB7       | USART1 RX      |

The final profile disables the human debug UART.

## Build and upload

```bash
pio run -d fairy -e nucleo_g071rb_auto_rs485
pio run -d fairy -e nucleo_g071rb_auto_rs485 -t upload
```

Build the final pin profile:

```bash
pio run -d fairy -e final_pcb_g071gb
```

Flash all stm32 boards:

```bash
python scripts/flash_all.py --jobs 1
```

## Timing

The HSI16 and PLL create a 64 MHz system clock. TIM2 prescaler 3 creates the required 16 MHz free running counter. A 32 bit overflow extension produces 64 bit timestamps.

SYNC captures the falling edge because Korora generates an active low pulse. Light gate captures the rising edge for beam interruption.

## Light gate

Startup:

1. Keep the IR emitter off
2. Measure the pulled high dark value
3. Enable the emitter
4. Measure the clear slot value
5. Require at least 400 ADC counts of separation
6. Emit a critical fault when the test fails

The emitter returns to off after the startup test and is enabled when a session starts. A 100 ms digital debounce suppresses small tremor edges while keeping the expected mouse snout event rate.

## Audio

Audio mode 1 is a tone. Audio mode 2 is band limited white noise. Initial values in Adelie are 8000 Hz to 16000 Hz and amplitude 512.

The timer period gives approximately 95952 samples per second from the 64 MHz clock. DMA uses a 256 sample circular buffer with half buffer callbacks.

## Valve

Initial 5 V values are:

| Setting          |         Value |
| ---------------- | ------------: |
| Spike duration   |      12000 us |
| Spike duty       | 660 per mille |
| Hold duty        | 400 per mille |
| Hard maximum     |     250000 us |
| Minimum interval |     250000 us |
