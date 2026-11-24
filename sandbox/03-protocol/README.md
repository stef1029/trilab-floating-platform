# 03 — The real protocol, end to end

```
PC  ──BLE──>  nRF52840 DK  ──UART 460800──>  Nucleo G071RB  ──>  LED
 adelie.transport        rig::transport         rig::transport
```

**Genuine Transport v1 at every hop** — 12-byte header, CRC16-CCITT, COBS
framing on the UART, real addresses from `system_config.hpp`. Both firmwares
link `firmware/shared`; the host imports `adelie.transport`. One
implementation of the protocol, three consumers.

Cross-checked: a frame built by Python's `encode_frame` decodes with the C++
`decode_frame`, same CRC and addresses.

## Wiring (3.3 V logic, no RS485 transceivers yet)

| nRF52840 DK | | Nucleo G071RB |
| --- | --- | --- |
| P0.27 (UART1 TX) | → | PC5 / D0 (USART1 RX) |
| P0.26 (UART1 RX) | ← | PC4 / D1 (USART1 TX) |
| GND | ↔ | GND |

Same pins both real firmwares use, so this carries over when the RS485
transceivers go in.

## Run

```sh
../sb bridge build && ../sb bridge flash
../sb node  build && ../sb node  flash
cd host && run            # Windows cmd;  ./run.sh in git bash
```

**Ping node 0** first — it exercises the whole path with no side effect.

Consoles: bridge on the DK's VCOM0, node on the ST-Link VCOM, both 115200.
Each prints every frame it sees.

## Real vs stubbed

**Real** — the header, CRC16, COBS, addresses (`0x03` Adelie, `0x10` node 0),
transfer-ID correlation, request/response flags, the ISR→main-loop handoff on
both boards.

**Stubbed** — payload is `[opcode, value]` rather than Command v2 TLV; the
address is hardcoded instead of assigned by Discovery; no fragmentation; no
RS485 direction control.

Each is a separate next step, and **none changes the layer below**.
