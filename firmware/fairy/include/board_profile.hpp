#pragma once

#include <cstdint>

#include <stm32g0xx_hal.h>

extern "C" void Error_Handler();

#ifndef FAIRY_BOARD_PROFILE
#define FAIRY_BOARD_PROFILE 1
#endif

#ifndef FAIRY_LIGHT_GATE_ALWAYS_ON
#define FAIRY_LIGHT_GATE_ALWAYS_ON 0
#endif

namespace fairy_board {

inline constexpr std::uint32_t profile = FAIRY_BOARD_PROFILE;
inline constexpr bool prototype = profile == 1;
inline constexpr bool final_pcb = profile == 2;
static_assert(prototype || final_pcb, "unknown Fairy board profile");

inline constexpr const char *name =
    prototype ? "nucleo_g071rb_auto_rs485" : "fairy_pcb_g071gb";

inline constexpr std::uint32_t common_timer_hz = 16'000'000;
inline constexpr std::uint32_t system_clock_hz = 64'000'000;

/*
 * Set only for a legacy prototype whose emitter is wired permanently on.
 * The current EE-SX1140 circuit drives a 2N7000 gate from PC1/PA3, so both the
 * prototype and final PCB use controlled emitter-off/emitter-on calibration.
 */
inline constexpr bool light_gate_always_on = FAIRY_LIGHT_GATE_ALWAYS_ON != 0;
inline constexpr std::uint16_t light_gate_clear_adc = 300;
inline constexpr std::uint16_t light_gate_blocked_adc = 600;
inline constexpr std::uint32_t light_gate_sample_interval_us = 1000;
static_assert(light_gate_clear_adc < light_gate_blocked_adc,
              "light gate hysteresis thresholds are reversed");

/*
 * Both current prototype UART pins use the automatic direction transceiver.
 * No GPIO direction signal is driven in profile 1.
 */
inline constexpr bool rs485_has_direction = final_pcb;

void enable_gpio_clocks();
void initialize_safe_gpio();
void initialize_alternate_functions();

void set_rs485_transmit(bool enabled);
void set_amplifier_enabled(bool enabled);
void set_ir_enabled(bool enabled);

} // namespace fairy_board
