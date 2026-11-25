# Node firmware

Node runs on STM32G071. It supports the NUCLEO G071RB prototype and the planned STM32G071GBU6 board through one board profile file.

## Responsibilities

- Read its node number from the identity page and use RS485 address 0x10 + number
- Report the 96 bit STM32 factory UUID in its boot record, for information only
- Capture 4 Hz SYNC on TIM2 channel 1
- Capture light gate interruption on TIM2 channel 2
- Debounce light gate edges for 100 ms
- Buffer node v3 records until Hub acknowledges them
- Drive RGB at 2 kHz
- Generate tone or band limited white noise with DAC DMA
- Apply a 10 ms audio onset and offset ramp
- Drive the non-latching valve at full duty for the whole open time
- Enforce valve safety limits
- Run an independent watchdog
- Perform a light gate startup self test
- Stop the local session after two seconds without Hub contact

## Select a profile

`NODE_BOARD_PROFILE=1` selects the Nucleo prototype.

`NODE_BOARD_PROFILE=2` selects the final package pin plan.

All pin decisions are in:

```text
include/board_profile.hpp
src/board_profile.cpp
```

This is the intended place to change a prototype pin.

## Prototype pins

The user supplied PC4 and PC5 choice is used for USART1. The automatic
direction Grove module means no direction pin is driven in this profile.
The final PCB profile drives PB3 high only while transmitting.

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
pio run -d reward_port -e nucleo_g071rb_auto_rs485
pio run -d reward_port -e nucleo_g071rb_auto_rs485 -t upload
```

Build the final pin profile:

```bash
pio run -d reward_port -e final_pcb_g071gb
```

## Flash a board and give it its number

Every board has a node number, 0 to 4 (`RIG_MAX_NODES - 1`). The number is
the board's identity: node 3 is always RS485 address 0x13. Flash one board at
a time with one ST-Link connected:

```bash
python scripts/flash.py --id 3   # new board, or renumbering: firmware + number
python scripts/flash.py          # firmware update, keeps the board's number
python scripts/flash.py --read   # report number and UUID, change nothing
```

Write the number on the board after giving it one. Plain `flash.py` refuses a
board that has no number yet, so a fresh board always gets one on its first
flash.

The number lives in the last 2 KB flash page (0x0801F800), which
`ld/node_flash.ld` keeps outside the firmware image. Any firmware-only flash
(`flash.py` without `--id`, `flash_all.py`, or PlatformIO upload) leaves it
alone. A full chip erase removes it. The record format is in
`../shared/include/rig_protocol/node_identity.hpp`.

A board with no valid number stays silent on RS485 and blinks red once a
second.

Two boards with the same number will collide on the bus. Keep the numbers
unique; `flash.py --read` shows a board's number before you plug it in.

To update several boards at once with the same image, `flash_all.py` still
works and keeps each board's number:

```bash
python scripts/flash_all.py --jobs 1
```

## Timing

The HSI16 and PLL create a 64 MHz system clock. TIM2 prescaler 3 creates the required 16 MHz free running counter. A 32 bit overflow extension produces 64 bit timestamps.

SYNC captures the falling edge because Hub generates an active low pulse. Light gate captures the rising edge for beam interruption.

## Light gate

Startup:

1. Keep the IR emitter off
2. Measure the pulled high dark value
3. Enable the emitter
4. Measure the clear slot value
5. Require at least 400 ADC counts of separation
6. Emit a critical fault when the test fails

If the test passes, the emitter stays on from boot and the gate reports beam breaks whether or not a session is running. Making the outputs safe (session start or stop, Hub's boot-time release, communication timeout) leaves it on. A `set IR` command can still switch it off until the next of those. If the test fails, the emitter stays off. A 100 ms digital debounce suppresses small tremor edges while keeping the expected mouse snout event rate.

## Audio

Audio mode 1 is a tone. Audio mode 2 is band limited white noise. Initial values in Adelie are 8000 Hz to 16000 Hz and amplitude 512.

The timer period gives approximately 48012 samples per second from the 64 MHz clock. DMA uses a 256 sample circular buffer, and each half is refilled directly in the DMA half and full callbacks.

## Valve

The valve is normally closed and non-latching. DRV8837 IN2 is hard wired to ground, so IN1 is driven at 100% for the whole requested open time and the spring closes the valve when drive is removed. There is no PWM hold phase. The spike and hold fields in the valve configuration are still accepted for protocol compatibility but are ignored.

| Setting                  |     Value |
| ------------------------ | --------: |
| Maximum open time        | 250000 us |
| Default minimum interval | 250000 us |
| Lowest minimum interval  |  50000 us |
