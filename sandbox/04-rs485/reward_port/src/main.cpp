/*
 * Sandbox "Node": the far end of the real data path.
 *
 *   PC --BLE--> nRF52840 --USART1 460800--> THIS BOARD --> LED
 *
 * Frames on the wire are genuine Transport v1, encoded and decoded by the
 * same firmware/shared library the production firmware uses: 12-byte header,
 * CRC16-CCITT, COBS framing, zero terminator.
 *
 * Wiring (3.3V logic, no RS485 transceivers yet):
 *     DK P0.27 (TX) -> Nucleo PC5 / D0   (USART1 RX)
 *     DK P0.26 (RX) <- Nucleo PC4 / D1   (USART1 TX)
 *     GND <-> GND
 *
 * The console stays on USART2 / ST-Link VCOM at 115200.
 */
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <stm32g0xx_hal.h>

#include "rig_protocol/system_config.hpp"
#include "rig_protocol/transport.hpp"

extern "C" void Error_Handler();

namespace {

/* ---------------------------------------------------------- our identity --
 * Real addresses from the shared config, so a frame built by Host's
 * transport.py addresses this board exactly as it would a real node 0.
 */
constexpr std::uint8_t my_address = rig::config::node_address(0);   /* 0x10 */

/* A minimal application payload. Transport v1 carries it verbatim; the real
 * system would put Host v2 TLV here instead. */
enum class SandboxOp : std::uint8_t {
    ping = 0x01,
    set_led = 0x02,
};

GPIO_TypeDef *const led_port = GPIOA;         /* LD4, the green user LED */
constexpr std::uint16_t led_pin = GPIO_PIN_5;

UART_HandleTypeDef console;                   /* USART2 -> ST-Link VCOM   */
UART_HandleTypeDef link;                      /* USART1 -> the nRF52840   */

/* ISR -> main-loop handoff, exactly the pattern from the real rs485_link. */
std::uint8_t link_byte;
std::array<std::uint8_t, 512> receive_ring;
volatile std::uint16_t receive_read;
volatile std::uint16_t receive_write;

rig::transport::UartDecoder decoder;
std::uint32_t frames_ok;
std::uint32_t frames_bad;

/* ------------------------------------------------------------- plumbing --*/
void system_clock_initialize() {
  RCC_OscInitTypeDef oscillator{};
  oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  oscillator.HSIState = RCC_HSI_ON;
  oscillator.HSIDiv = RCC_HSI_DIV1;
  oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  oscillator.PLL.PLLState = RCC_PLL_ON;
  oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  oscillator.PLL.PLLM = RCC_PLLM_DIV1;
  oscillator.PLL.PLLN = 8;                    /* 16 MHz x 8 / 2 = 64 MHz */
  oscillator.PLL.PLLP = RCC_PLLP_DIV2;
  oscillator.PLL.PLLQ = RCC_PLLQ_DIV2;
  oscillator.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) { Error_Handler(); }

  RCC_ClkInitTypeDef clock{};
  clock.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1;
  clock.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clock.APB1CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_2) != HAL_OK) { Error_Handler(); }
}

void led_initialize() {
  __HAL_RCC_GPIOA_CLK_ENABLE();
  HAL_GPIO_WritePin(led_port, led_pin, GPIO_PIN_RESET);   /* value, then mode */
  GPIO_InitTypeDef gpio{};
  gpio.Pin = led_pin;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(led_port, &gpio);
}

void console_initialize() {
  __HAL_RCC_USART2_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  GPIO_InitTypeDef gpio{};
  gpio.Pin = GPIO_PIN_2 | GPIO_PIN_3;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF1_USART2;
  HAL_GPIO_Init(GPIOA, &gpio);

  console.Instance = USART2;
  console.Init.BaudRate = 115200;
  console.Init.WordLength = UART_WORDLENGTH_8B;
  console.Init.StopBits = UART_STOPBITS_1;
  console.Init.Parity = UART_PARITY_NONE;
  console.Init.Mode = UART_MODE_TX_RX;
  console.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  console.Init.OverSampling = UART_OVERSAMPLING_16;
  console.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&console) != HAL_OK) { Error_Handler(); }
}

void print(const char *text) {
  (void)HAL_UART_Transmit(&console, reinterpret_cast<const std::uint8_t *>(text),
                          static_cast<std::uint16_t>(std::strlen(text)), 100);
}

void logf(const char *format, ...) {
  char line[128];
  va_list args;
  va_start(args, format);
  (void)std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  print(line);
}

/*
 * Cortex-M0+ has no DWT cycle counter, so there is no free hardware
 * microsecond clock to read. The real node dedicates TIM2 to the job (see
 * firmware/reward_port/src/timebase.cpp). Here a NOP loop is enough, because the
 * only thing that depends on it is a guard that must be AT LEAST this long
 * -- erring long costs nothing, erring short corrupts a frame.
 */
void busy_wait_us(std::uint32_t microseconds) {
  /* Four cycles per iteration is a conservative under-estimate of the loop
   * body, so this waits somewhat longer than asked. That is the safe way to
   * be wrong. */
  const std::uint32_t loops =
      microseconds * (SystemCoreClock / 1000000U) / 4U;
  for (std::uint32_t i = 0; i < loops; ++i) {
    __NOP();
  }
}

/*
 * PB3 drives DE and /RE on the transceiver, exactly as the node PCB does
 * (firmware/reward_port/src/board_profile.cpp, set_rs485_transmit).
 *
 *   HIGH -> we drive the bus, our receiver is muted
 *   LOW  -> we listen
 */
void set_rs485_transmit(bool enabled) {
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3,
                    enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* PC4 = USART1 TX, PC5 = USART1 RX -- the same pins the real prototype uses. */
void link_initialize() {
  __HAL_RCC_USART1_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  GPIO_InitTypeDef gpio{};
  gpio.Pin = GPIO_PIN_4;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF1_USART1;
  HAL_GPIO_Init(GPIOC, &gpio);

  gpio.Pin = GPIO_PIN_5;
  gpio.Pull = GPIO_PULLUP;                    /* idle high when nobody drives */
  HAL_GPIO_Init(GPIOC, &gpio);

  /* The direction pin. Configure it BEFORE enabling the UART, and start it
   * low: a node that powers up driving the bus jams it for everyone. */
  __HAL_RCC_GPIOB_CLK_ENABLE();
  GPIO_InitTypeDef dir{};
  dir.Pin = GPIO_PIN_3;
  dir.Mode = GPIO_MODE_OUTPUT_PP;
  dir.Pull = GPIO_NOPULL;
  dir.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &dir);
  set_rs485_transmit(false);

  link.Instance = USART1;
  link.Init.BaudRate = rig::config::rs485_baud;   /* 460800 */
  link.Init.WordLength = UART_WORDLENGTH_8B;
  link.Init.StopBits = UART_STOPBITS_1;
  link.Init.Parity = UART_PARITY_NONE;
  link.Init.Mode = UART_MODE_TX_RX;
  link.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  link.Init.OverSampling = UART_OVERSAMPLING_16;
  link.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&link) != HAL_OK) { Error_Handler(); }

  HAL_NVIC_SetPriority(USART1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(USART1_IRQn);
  if (HAL_UART_Receive_IT(&link, &link_byte, 1) != HAL_OK) { Error_Handler(); }
}

/* --------------------------------------------------------- the protocol --*/
void send_response(const rig::transport::MessageView &request,
                   const std::uint8_t *payload, std::size_t length) {
  rig::transport::Header header;
  header.channel = request.channel;
  header.flags = rig::transport::response | rig::transport::first_fragment |
                 rig::transport::last_fragment;
  header.source = my_address;
  header.destination = request.source;
  header.fragment_index = 0;
  header.fragment_count = 1;
  header.transfer_id = request.transfer_id;

  std::uint8_t raw[rig::transport::max_raw_frame_size]{};
  const std::size_t raw_length =
      rig::transport::encode_frame(header, payload, length, raw, sizeof(raw));
  if (raw_length == 0U) { return; }

  std::uint8_t encoded[rig::transport::max_encoded_uart_frame]{};
  const std::size_t encoded_length =
      rig::transport::cobs_encode(raw, raw_length, encoded, sizeof(encoded));
  if (encoded_length == 0U) { return; }

  /*
   * Turnaround guard: the bridge stopped driving only microseconds ago and
   * its transceiver needs time to release the pair. Talking over the tail of
   * its frame corrupts both.
   */
  busy_wait_us(40);
  set_rs485_transmit(true);
  busy_wait_us(1);    /* let DE settle before the first start bit */

  /*
   * HAL_UART_Transmit is BLOCKING, and crucially it waits on the TC
   * (transmission complete) flag, not merely TXE. TXE means "the holding
   * register is free", which is true while the last byte is still being
   * shifted out; TC means the stop bit has actually left the pin. Dropping
   * DE on TXE would truncate the final character.
   *
   * That is why the node can get away with a blocking call here while the
   * bridge needs UART_TX_DONE: the STM32 HAL already waits for the right
   * flag, so the line after it is the right moment.
   */
  (void)HAL_UART_Transmit(&link, encoded,
                          static_cast<std::uint16_t>(encoded_length), 20);

  set_rs485_transmit(false);
}

void handle_message(const rig::transport::MessageView &message) {
  if (message.destination != my_address &&
      message.destination != rig::config::broadcast_address) {
    return;                                   /* not for us */
  }
  if (message.payload_length < 1U) { return; }

  const auto op = static_cast<SandboxOp>(message.payload[0]);
  switch (op) {
  case SandboxOp::set_led: {
    if (message.payload_length < 2U) { return; }
    const bool on = message.payload[1] != 0U;
    HAL_GPIO_WritePin(led_port, led_pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    logf("SET_LED %u from=0x%02x transfer=%u\r\n", on ? 1U : 0U,
         message.source, message.transfer_id);
    const std::uint8_t reply[2] = {static_cast<std::uint8_t>(SandboxOp::set_led),
                                   static_cast<std::uint8_t>(on ? 1U : 0U)};
    send_response(message, reply, sizeof(reply));
    break;
  }
  case SandboxOp::ping: {
    logf("PING from=0x%02x transfer=%u\r\n", message.source,
         message.transfer_id);
    const std::uint8_t reply[1] = {static_cast<std::uint8_t>(SandboxOp::ping)};
    send_response(message, reply, sizeof(reply));
    break;
  }
  default:
    logf("UNKNOWN op=0x%02x\r\n", message.payload[0]);
    break;
  }
}

void consume_received() {
  while (receive_read != receive_write) {
    const std::uint8_t byte = receive_ring[receive_read];
    receive_read = static_cast<std::uint16_t>((receive_read + 1U) %
                                              receive_ring.size());

    rig::transport::FrameView frame;
    const auto result = decoder.push(byte, frame);
    if (result == rig::transport::DecodeResult::ok) {
      ++frames_ok;
      /* One fragment per frame here, so a FrameView is already a message. */
      rig::transport::MessageView message;
      message.channel = frame.header.channel;
      message.flags = frame.header.flags;
      message.source = frame.header.source;
      message.destination = frame.header.destination;
      message.transfer_id = frame.header.transfer_id;
      message.payload = frame.payload;
      message.payload_length = frame.payload_length;
      handle_message(message);
    } else if (result != rig::transport::DecodeResult::incomplete) {
      ++frames_bad;
      logf("BAD_FRAME result=%u ok=%lu bad=%lu\r\n",
           static_cast<unsigned>(result),
           static_cast<unsigned long>(frames_ok),
           static_cast<unsigned long>(frames_bad));
    }
  }
}

} // namespace

int main() {
  HAL_Init();
  system_clock_initialize();
  led_initialize();
  console_initialize();
  link_initialize();

  logf("\r\nnode-lite ready: address=0x%02x baud=%lu\r\n",
       my_address, static_cast<unsigned long>(rig::config::rs485_baud));
  print("  USART1 PC4=TX PC5=RX, console on USART2 115200\r\n");

  while (true) {
    consume_received();
    __WFI();
  }
}

extern "C" void SysTick_Handler() { HAL_IncTick(); }

extern "C" void USART1_IRQHandler() { HAL_UART_IRQHandler(&link); }

extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef *handle) {
  if (handle != &link) { return; }
  const std::uint16_t next =
      static_cast<std::uint16_t>((receive_write + 1U) % receive_ring.size());
  if (next != receive_read) {
    receive_ring[receive_write] = link_byte;
    __DMB();                                  /* publish data, then the index */
    receive_write = next;
  }
  (void)HAL_UART_Receive_IT(&link, &link_byte, 1);
}

extern "C" void Error_Handler() {
  __disable_irq();
  while (true) {
  }
}
