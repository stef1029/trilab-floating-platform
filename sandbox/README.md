# Sandbox

Scratch workspace for rebuilding the system one feature at a time. Nothing in
here is used by `firmware/` — break it freely.

| Project | Board | Toolchain |
| --- | --- | --- |
| `blink-nordic` | nRF52840 DK | Zephyr / nRF Connect SDK v3.4.1 |
| `blink-stm32` | NUCLEO-G071RB | PlatformIO + STM32Cube |

## Quick start

```sh
cd sandbox

./blink nordic build      # build for the nRF52840 DK
./blink nordic flash      # flash it (DK plugged in via USB)

./blink stm32 build       # build for the NUCLEO-G071RB
./blink stm32 flash       # flash it
./blink stm32 monitor     # serial console, 115200
```

Expected result on both boards: an LED blinking at 1 Hz, and a `toggle N`
line on the console for each blink.

## Where things live

```
sandbox/
├── blink                 # build/flash helper for both toolchains
├── blink-nordic/
│   ├── CMakeLists.txt    # which sources to compile
│   ├── prj.conf          # which Zephyr features to enable
│   └── src/main.c
└── blink-stm32/
    ├── platformio.ini    # board, flags, upload method
    └── src/main.cpp
```

## Toolchain locations

Neither toolchain is on `PATH`. Both are invoked through wrappers.

| | Path |
| --- | --- |
| nRF Connect SDK | `C:\ncs\v3.4.1` |
| Nordic toolchain | `C:\ncs\toolchains\4f5b6ad6dd` |
| `nrfutil` | `C:\ncs\tools\nrfutil.exe` |
| PlatformIO | `~/.platformio/penv/Scripts/pio.exe` |
| ARM GCC (PlatformIO) | `~/.platformio/packages/toolchain-gccarmnoneeabi/bin/` |
| OpenOCD | `~/.platformio/packages/tool-openocd/bin/` |

### Raw commands, if you want to skip the script

Nordic — needs `ZEPHYR_BASE` because the app lives outside the SDK workspace:

```sh
export ZEPHYR_BASE='C:/ncs/v3.4.1/zephyr'
/c/ncs/tools/nrfutil.exe sdk-manager toolchain launch --ncs-version v3.4.1 -- \
    west build -p always -b nrf52840dk/nrf52840 blink-nordic --build-dir blink-nordic/build
```

STM32:

```sh
~/.platformio/penv/Scripts/pio.exe run -d blink-stm32
~/.platformio/penv/Scripts/pio.exe run -d blink-stm32 -t upload
```

## The two toolchains, side by side

| | Nordic (Zephyr) | STM32 (PlatformIO) |
| --- | --- | --- |
| Add a source file | list it in `CMakeLists.txt` | drop it in `src/`, found automatically |
| Enable a driver | `CONFIG_GPIO=y` in `prj.conf` | always available via the HAL |
| Pick a pin | devicetree (`DT_ALIAS(led0)`) | named in code (`GPIOA`, `GPIO_PIN_5`) |
| Change board | `-b <board>` | `board =` in `platformio.ini` |
| Clean rebuild | `-p always` | `pio run -t clean` |
| Build output | `build/blink-nordic/zephyr/zephyr.elf` | `.pio/build/nucleo_g071rb/firmware.elf` |
| Console | J-Link VCOM, 115200 | ST-Link VCOM, 115200 |

## Useful things

```sh
# What's connected?
~/.platformio/penv/Scripts/pio.exe device list

# Inspect a build
BIN=~/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi
$BIN-size      blink-stm32/.pio/build/nucleo_g071rb/firmware.elf
$BIN-nm -S -C --size-sort blink-stm32/.pio/build/nucleo_g071rb/firmware.elf | tail -20

# What did devicetree actually produce? (Nordic)
less blink-nordic/build/blink-nordic/zephyr/zephyr.dts

# What Kconfig symbols ended up set? (Nordic)
grep GPIO blink-nordic/build/blink-nordic/zephyr/.config

# Browse Kconfig interactively
./blink nordic build     # then:
/c/ncs/tools/nrfutil.exe sdk-manager toolchain launch --ncs-version v3.4.1 -- \
    west build --build-dir blink-nordic/build -t menuconfig
```

## Editor IntelliSense (`cannot open source file "stm32g0xx_hal.h"`)

That error is VS Code, not the compiler - the build is fine. The C/C++
extension needs a **compilation database** telling it where the headers are.

```sh
./blink stm32 index      # regenerates blink-stm32/compile_commands.json
./blink nordic build     # Zephyr generates one automatically
```

`.vscode/c_cpp_properties.json` already points at both. Pick which one is
active with **Ctrl+Shift+P -> C/C++: Select a Configuration**:

| Configuration | For files in |
| --- | --- |
| `sandbox-stm32` | `sandbox/blink-stm32/` |
| `sandbox-nordic` | `sandbox/blink-nordic/` |
| `firmware-fairy` | `firmware/fairy/` (run `pio run -d firmware/fairy -t compiledb` first) |

If errors persist after switching: **Ctrl+Shift+P -> C/C++: Reset IntelliSense
Database**. Regenerate the index whenever you add a source file or change
include paths.

## Planned build-up

1. **Blink** — toolchain works, board flashes, console prints. ← you are here
2. **UART** — talk to the PC, structured output
3. **Timer + SYNC generation** — TIMER2 → PPI → GPIOTE
4. **RS485** — UART + direction pin, then framing
5. **Magellan** — discovery and addressing against a real Fairy
6. **Clock model** — SYNC capture, pairing, the affine fit
7. **BLE** — advertise, connect, the Adelie service
8. **Second BLE link** — Galapagos, anchor points, TTL

## ble-led — a stripped-down Adelie/Korora link

The smallest thing that is still recognisably the real system: a GUI button
turns an LED on, and the board's button reports back.

```
GUI toggle        --  BLE write         -->  LED1 on the DK
BUTTON1 on the DK --  BLE notification  -->  counter in the GUI
```

```sh
./blink ble build
./blink ble flash
cd ble-host
run                     # Windows cmd
./run.sh                # git bash / PowerShell
```

Click **Connect**, then the LED button.

### What it leaves out

Everything above the transport. Here the characteristic value *is* the whole
message; real Korora wraps both directions in Transport v1 — 12-byte header,
CRC16, fragmentation, channel byte — then Adelie v2 or Fairy v3 on top.

### The pieces that do match

| | `ble-led` | Korora |
| --- | --- | --- |
| Role | peripheral | peripheral (to Adelie) + central (to Galapagos) |
| Service | one custom 128-bit UUID | `a88279d0-…` |
| Host → board | write characteristic | RX characteristic `…d1` |
| Board → host | notify characteristic | TX characteristic `…d2` |
| On disconnect | LED forced off | outputs safe, session ends |
| MTU | 247 requested | 247 requested |

## IntelliSense: set up once, never think about it again

This repo uses **clangd** rather than the Microsoft C/C++ extension's parser.
clangd reads each project's `compile_commands.json` and picks the right one
**per file** — so opening a Zephyr file and then an STM32 file just works,
with no configuration to switch.

**One-time setup:** install the **clangd** extension
(`llvm-vs-code-extensions.vscode-clangd`) and reload. The clangd binary
itself is already installed at
`%LOCALAPPDATA%/clangd/clangd_18.1.3/bin/clangd.exe` and named in
`.vscode/settings.json`, which also disables the competing parser.

**Then the only rule is: build a project before editing it.** The database is
generated by the build, so a project that has never been built has no index.

```sh
./blink ble build          # or nordic / stm32
./blink stm32 index        # PlatformIO needs the database asked for explicitly
```

Zephyr nests its database deeper than clangd searches, so each app has a
`.clangd` file naming the build directory:

```yaml
CompileFlags:
  CompilationDatabase: build-dk/ble-led
```

Point that at a different build directory to index a different board —
`firmware/korora/.clangd` currently indexes the DK build rather than the
BMD-340 one.

## bridge + fairy-lite — the real protocol, end to end

```
PC  ──BLE──>  nRF52840 DK  ──UART 460800──>  Nucleo G071RB  ──>  LED
 adelie.transport        fairy::transport         fairy::transport
```

All three ends use **genuine Transport v1** — 12-byte header, CRC16-CCITT,
COBS framing on the UART, real addresses from `system_config.hpp`. The C++
side links `firmware/shared` directly; the Python side imports
`adelie.transport`. Verified by round-trip: a frame built in Python decodes
with the C++ library.

### Wiring (3.3 V logic, no RS485 transceivers yet)

| nRF52840 DK | | Nucleo G071RB |
| --- | --- | --- |
| P0.27 (UART1 TX) | → | PC5 / D0 (USART1 RX) |
| P0.26 (UART1 RX) | ← | PC4 / D1 (USART1 TX) |
| GND | ↔ | GND |

### Run

```sh
./blink bridge build && ./blink bridge flash      # nRF52840 DK
./blink fairy  build && ./blink fairy  flash      # Nucleo

cd ble-host
run_fairy            # Windows cmd
./run_fairy.sh       # git bash
```

Consoles: bridge on the DK's VCOM0 (115200), fairy-lite on the ST-Link VCOM
(115200). Both print each frame as it passes.

### The payload

Transport v1 carries two bytes: `[opcode, value]`, with `0x02 0x01` meaning
"LED on". The real system puts **Adelie v2 TLV** here instead — that is the
next layer to add, and nothing below it changes.
