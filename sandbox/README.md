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
