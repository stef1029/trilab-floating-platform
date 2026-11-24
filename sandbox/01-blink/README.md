# 01 — Blink

Prove the toolchain, the board and the console all work, on both architectures.

```sh
../sb nordic build && ../sb nordic flash     # nRF52840 DK
../sb stm32  build && ../sb stm32  flash     # NUCLEO-G071RB
../sb stm32  monitor                         # 115200
```

Expect a 1 Hz LED and a `toggle N` line per blink.

| | `nordic/` | `stm32/` |
| --- | --- | --- |
| Build system | Zephyr / CMake / west | PlatformIO |
| Add a source file | list it in `CMakeLists.txt` | drop it in `src/` |
| Enable a driver | `CONFIG_GPIO=y` in `prj.conf` | always there via the HAL |
| Pick a pin | devicetree, `DT_ALIAS(led0)` | named in code, `GPIOA` / `GPIO_PIN_5` |
| Waiting | `k_msleep()` — yields the CPU | `HAL_Delay()` — busy-waits |
| Flash / RAM | 2.21% / 2.32% | 6.00% / 0.84% |

The size difference is the kernel: Zephyr brings a scheduler and a console
subsystem, the STM32 build brings nothing you did not write.
