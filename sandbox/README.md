# Sandbox

Rebuilding the system one layer at a time. Nothing here is used by
`firmware/` — break it freely.

## Lessons

| | What | Boards |
| --- | --- | --- |
| **[01-blink](01-blink/)** | toolchain, flashing, console, both architectures | DK + Nucleo |
| **[02-ble](02-ble/)** | GATT service, write in / notify out, a Python host | DK |
| **[03-protocol](03-protocol/)** | real Transport v1 across BLE and UART | DK + Nucleo |

### Planned

4. **RS485** — swap the direct UART for transceivers and a direction pin
5. **Adelie v2** — TLV payloads instead of `[opcode, value]`
6. **Magellan** — discovery and address assignment
7. **Timer + SYNC** — TIMER2 → PPI → GPIOTE, hardware-timed pulses
8. **Clock model** — SYNC capture, pairing, the affine fit
9. **Galapagos** — second BLE link, anchor points, TTL

## The `sb` script

```sh
./sb <target> [build|flash|monitor|index]        # default action: build

./sb nordic      01-blink/nordic        nRF52840 DK
./sb stm32       01-blink/stm32         NUCLEO-G071RB
./sb ble         02-ble/peripheral      nRF52840 DK
./sb bridge      03-protocol/bridge     nRF52840 DK
./sb fairy       03-protocol/fairy      NUCLEO-G071RB
```

It wraps the toolchains so neither needs to be on `PATH`. Raw commands below
if you would rather drive them yourself.

## Hardware on this bench

```
1057715617   PCA10156  nRF54L15-DK    COM11, COM12
1050230658   PCA10056  nRF52840-DK    COM13, COM14      <- DK_SERIAL default
0668FF4850…  STM32 ST-Link            COM15
```

Each Nordic DK exposes two COM ports; **VCOM0 is the console**. Override the
flash target with `DK_SERIAL=... ./sb ble flash`.

## Toolchains

| | Path |
| --- | --- |
| nRF Connect SDK | `C:\ncs\v3.4.1` |
| Nordic toolchain | `C:\ncs\toolchains\4f5b6ad6dd` |
| `nrfutil` | `C:\ncs\tools\nrfutil.exe` |
| PlatformIO | `~/.platformio/penv/Scripts/pio.exe` |
| ARM GCC (PlatformIO) | `~/.platformio/packages/toolchain-gccarmnoneeabi/bin/` |
| OpenOCD | `~/.platformio/packages/tool-openocd/bin/` |
| clangd | `%LOCALAPPDATA%/clangd/clangd_18.1.3/bin/clangd.exe` |

### Raw commands

Zephyr — `ZEPHYR_BASE` is needed because the apps live outside the SDK:

```sh
export ZEPHYR_BASE='C:/ncs/v3.4.1/zephyr'
/c/ncs/tools/nrfutil.exe sdk-manager toolchain launch --ncs-version v3.4.1 -- \
    west build -p always -b nrf52840dk/nrf52840 02-ble/peripheral \
               --build-dir 02-ble/peripheral/build-dk
```

PlatformIO:

```sh
~/.platformio/penv/Scripts/pio.exe run -d 01-blink/stm32 -t upload
~/.platformio/penv/Scripts/pio.exe device monitor -p COM15 -b 115200
```

### Useful

```sh
# what is connected?
~/.platformio/penv/Scripts/pio.exe device list
/c/ncs/tools/nrfutil.exe device list

# inspect a build
BIN=~/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi
$BIN-size 01-blink/stm32/.pio/build/nucleo_g071rb/firmware.elf
$BIN-nm -S -C --size-sort <elf> | tail -20

# what did devicetree produce?  what did Kconfig resolve to?
less 02-ble/peripheral/build-dk/peripheral/zephyr/zephyr.dts
grep BT_ 02-ble/peripheral/build-dk/peripheral/zephyr/.config

# where did the flash go?
./sb ble build   # then:
/c/ncs/tools/nrfutil.exe sdk-manager toolchain launch --ncs-version v3.4.1 -- \
    west build --build-dir 02-ble/peripheral/build-dk -t rom_report
```

## IntelliSense

This repo uses **clangd**, which reads each project's `compile_commands.json`
and picks the right one **per file** — no configuration to switch. The binary
is installed and named in `.vscode/settings.json`, which also disables the
competing C/C++ parser.

**The only rule: build a project before editing it.** The database is a build
artefact, so a project that has never been built has no index.

```sh
./sb ble build           # Zephyr generates one automatically
./sb stm32 index         # PlatformIO needs asking explicitly
```

Zephyr nests its database deeper than clangd searches, so each app has a
`.clangd` naming the build directory. Point it elsewhere to index a different
board.

## Shells

Commands here are bash. In **cmd.exe** use backslashes (`..\..\..\software\...`)
— it reads `/` as a flag marker. The `run.cmd` / `run.sh` pairs in each
`host/` folder resolve their own paths, so either shell works.
