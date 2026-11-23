# 04 — RS485

Module 03 ran Transport v1 over a bare wire: DK TX straight to Nucleo RX, and
back. That works on a desk and nowhere else. This module puts the real
physical layer underneath it.

Nothing above the wire changes. Same frames, same CRC, same COBS, same host
script. The whole lesson is in **when you are allowed to talk**.

## Why RS485 at all

| | UART on a wire | RS485 |
| --- | --- | --- |
| Signalling | one wire against ground | **differential pair**, A and B |
| Noise | picked up directly | cancels — both wires catch it equally |
| Distance | ~1 m before you worry | hundreds of metres |
| Nodes | 2 | up to 32 on one pair |

The receiver looks only at the *difference* between A and B, so interference
that hits both wires equally subtracts out. That is what lets one pair run
down a rig full of valves and solenoids without the frames dissolving.

A transceiver chip does the translation. The MCU still sees a plain UART:

```
   MCU                    transceiver                 the bus
   TX  ---------------->  DI                    A  ----+---------
   RX  <----------------  RO                    B  ----|----+----
   DE  ---------------->  DE and /RE                   |    |
                                                     other nodes
```

## The one hard part

RS485 is **half duplex**. One pair, everybody listening, one talker. The `DE`
pin decides who is driving:

- `DE` high → this node drives A/B, and its own receiver is muted
- `DE` low → this node listens

The rule is simple to state and easy to get wrong:

> **Raise DE before the first start bit. Drop it after the last stop bit.
> Not one bit earlier.**

Drop it early and the final character is cut off mid-transmission — the
receiver sees a truncated frame and the CRC rejects it. Drop it late and you
are still driving the bus when the other end starts replying, so two
transmitters fight over the same pair and both frames are destroyed.

And "after the last stop bit" is harder than it sounds, because the obvious
thing to do is wrong.

## Why `uart_poll_out` cannot be used here

Module 03 transmitted like this:

```cpp
for (size_t i = 0; i < encoded_length; ++i) {
    uart_poll_out(link, encoded[i]);
}
/* is the last byte on the wire yet?  No idea. */
```

`uart_poll_out` returns when the hardware has *accepted* the byte, which is
not the same as having *sent* it. The last character is still being shifted
out one bit at a time when the loop ends. Drop DE on the next line and you
truncate it.

There is no version of this loop that fixes the problem. You need the
hardware to tell you, and the two boards get told in different ways.

### The bridge: `UART_TX_DONE`

The nRF side switches to the **async (DMA) API**, which raises an event at
exactly the right moment:

```cpp
case UART_TX_DONE:
case UART_TX_ABORTED:
    gpio_pin_set_dt(&rs485_dir, 0);
    k_sem_give(&transmit_complete);
    break;
```

Note *where* DE is dropped — inside the callback, in interrupt context, not
back in the thread that called `uart_tx`. A Fairy may answer the instant it
sees the line idle. If DE stayed high until the scheduler got round to
waking the sending thread, we would still be driving while it replied.

`firmware/korora/src/rs485_bus.cpp` does precisely this, with the same
comment.

### The Fairy: the `TC` flag

The STM32 has no async API here, but it does not need one, because
`HAL_UART_Transmit` is blocking *and waits on the right flag*:

| Flag | Means |
| --- | --- |
| `TXE` | the holding register is free — the last byte is **still going out** |
| `TC` | transmission complete — the stop bit has **left the pin** |

`HAL_UART_Transmit` waits for `TC` before returning, so the line after it is
genuinely safe:

```cpp
set_rs485_transmit(true);
(void)HAL_UART_Transmit(&link, encoded, encoded_length, 20);
set_rs485_transmit(false);
```

Two platforms, two mechanisms, one requirement. Had the HAL waited only for
`TXE`, this code would be broken and would look identical.

## The turnaround guard

Both ends wait before asserting DE:

```cpp
busy_wait_us(40);          /* let the other end release the pair */
set_rs485_transmit(true);
busy_wait_us(1);           /* let DE settle before the first start bit */
```

The 40 µs is not arbitrary — it is what `firmware/fairy/src/rs485_link.cpp`
uses. A transceiver takes time to stop driving after DE falls, and starting
to drive into that tail corrupts both frames.

The 1 µs is the other side of the same coin: the driver needs a moment to
take control before the UART starts clocking bits out.

Erring long here costs a few microseconds. Erring short costs a frame.

## The DMA buffer trap

One change in the bridge that is easy to miss:

```cpp
static uint8_t encoded[fairy::transport::max_encoded_uart_frame];
```

It used to be a stack local, and with `uart_poll_out` that was fine — every
byte was copied into the hardware before the function returned.

`uart_tx` hands the DMA a **pointer** and returns immediately. The hardware
reads that memory over the following milliseconds. A stack buffer would be
long dead, and you would transmit whatever the next function call left
there — a bug that produces *occasional* corruption depending on timing,
which is the worst kind.

File scope is safe here only because one thread transmits. The real Korora
has the same constraint and solves it the same way, with a single RS485
manager thread owning the buffer.

## Wiring

Two transceiver breakouts, one bus:

```
   DK P0.27  -> DI        A ---+--- A   NUCLEO PC4 -> DI
   DK P0.26  <- RO        B ---|--- B   NUCLEO PC5 <- RO
   DK P1.12  -> DE//RE         |        NUCLEO PB3 -> DE//RE
   DK GND    -- GND            |        NUCLEO GND -- GND
                          120R terminators at each end
```

Grounds must be common. Differential does not mean floating — the receivers
still need both inputs inside their common-mode range.

**Termination**: 120 Ω across A/B at each *end* of the bus, nowhere in the
middle. On a 30 cm desk link at 460800 baud you will probably get away
without it; on the real rig you will not.

**Idle biasing**: with nobody driving, A and B float and the receiver output
is undefined — it can chatter and generate garbage bytes. Most breakouts
include bias resistors. If yours does not, and you see stray bytes when the
bus is quiet, that is why.

### If your breakout has no DE pin

Some modules — including the Grove RS-485 breakout used on the breadboard
rig — switch direction automatically by watching for activity on DI. They
work, but they guess at the turnaround and the guess gets worse as baud
rises.

The code still runs; DE just drives an unconnected pin. You lose the precise
turnaround control this module is about, which is exactly why the production
PCB has an explicit DE line.

## Build and run

```sh
./sb bridge485 build  &&  ./sb bridge485 flash
./sb fairy485  build  &&  ./sb fairy485  flash
python host/fairy_led.py
```

Same host script as module 03 — the PC cannot tell the difference, which is
the point.

## What to look for

The bridge console prints a count of frames each way. If the LED toggles and
both counts climb together, turnaround is working.

Failure modes, and what they mean:

| Symptom | Cause |
| --- | --- |
| Command works, no response | Fairy's DE stuck high, or never asserted |
| bad frame, `result=7` (bad CRC) | truncated frame — DE dropped too early |
| Garbage bytes when idle | no idle biasing on the bus |
| Works slow, fails at 460800 | auto-direction transceiver, or no termination |

`result=7` is `bad_crc` in the `DecodeResult` enum. Counting the entries in
`transport.hpp` is quicker than guessing.

## Next

**05 — Adelie v2**: replace the two-byte `[opcode, value]` payload with real
TLV records, so the sandbox speaks the actual application protocol rather
than a stand-in.
