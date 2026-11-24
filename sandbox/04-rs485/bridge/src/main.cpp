/*
 * Sandbox "Hub": a BLE <-> UART bridge speaking real Transport v1.
 *
 *   PC --BLE--> THIS BOARD --USART1 460800--> node-lite --> LED
 *
 * The PC writes a complete Transport v1 frame to the RX characteristic.
 * This board decodes it, checks the destination, and forwards it down the
 * UART with COBS framing. Responses travel back the same way and are
 * notified to the PC.
 *
 * Framing uses firmware/shared, the same library the production firmware
 * and node-lite use -- so the bytes on both links are genuinely the real
 * protocol, not a lookalike.
 *
 * Module 04 puts an RS485 transceiver on each end. The bus is half duplex:
 * one pair of wires, everyone listening, one talker at a time. A direction
 * pin (DE) decides who drives it, and the timing of that pin is the lesson.
 *
 * Wiring: DK P0.27 -> DI,  P0.26 <- RO,  P1.12 -> DE//RE,  A/B to the bus
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include "rig_protocol/system_config.hpp"
#include "rig_protocol/transport.hpp"

namespace {

/* Same UUID family as ble-led, next in the series. */
#define BRIDGE_SERVICE_UUID \
	BT_UUID_128_ENCODE(0x5b1e0011, 0xf00d, 0x4a5e, 0x9c33, 0x0a1b2c3d4e5f)
#define BRIDGE_RX_UUID \
	BT_UUID_128_ENCODE(0x5b1e0012, 0xf00d, 0x4a5e, 0x9c33, 0x0a1b2c3d4e5f)
#define BRIDGE_TX_UUID \
	BT_UUID_128_ENCODE(0x5b1e0013, 0xf00d, 0x4a5e, 0x9c33, 0x0a1b2c3d4e5f)

bt_uuid_128 service_uuid = BT_UUID_INIT_128(BRIDGE_SERVICE_UUID);
bt_uuid_128 rx_uuid      = BT_UUID_INIT_128(BRIDGE_RX_UUID);
bt_uuid_128 tx_uuid      = BT_UUID_INIT_128(BRIDGE_TX_UUID);

const device *const link = DEVICE_DT_GET(DT_NODELABEL(uart1));

/* DE and /RE tied together on one pin: HIGH drives the bus, LOW listens. */
const gpio_dt_spec rs485_dir =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), rs485_dir_gpios);

/* Raised by UART_TX_DONE so link_send knows the frame has really gone. */
K_SEM_DEFINE(transmit_complete, 0, 1);

/* Receive buffers for the DMA. Two, swapped as each fills, so the hardware
 * always has somewhere to put the next byte. */
uint8_t receive_buffers[2][64];
uint8_t next_receive_buffer;
bt_conn *current_conn;

/* UART ISR -> thread handoff. A message queue rather than node's hand-rolled
 * ring, because here there IS a scheduler to hand off to. */
K_MSGQ_DEFINE(link_bytes, sizeof(uint8_t), 1024, 1);

rig::transport::UartDecoder decoder;
uint32_t to_node;
uint32_t from_node;
uint32_t bad_frames;

/* ------------------------------------------------------------ UART side --*/
/* One callback for every UART event, transmit and receive alike. It runs in
 * interrupt context, so module 03's rule still applies: move data, get out,
 * never block. */
void link_event(const device *dev, uart_event *event, void *)
{
	switch (event->type) {
	case UART_RX_RDY:
		/* The DMA has landed some bytes. Note they sit at an OFFSET
		 * into the buffer, not at the start -- one buffer can be
		 * reported in several chunks. */
		for (size_t i = 0; i < event->data.rx.len; ++i) {
			const uint8_t byte =
				event->data.rx.buf[event->data.rx.offset + i];
			(void)k_msgq_put(&link_bytes, &byte, K_NO_WAIT);
		}
		break;

	case UART_RX_BUF_REQUEST:
		/* The current buffer is nearly full and the driver wants the
		 * next one NOW, while reception continues. This is what makes
		 * DMA receive gapless. */
		(void)uart_rx_buf_rsp(dev, receive_buffers[next_receive_buffer],
				      sizeof(receive_buffers[0]));
		next_receive_buffer ^= 1U;
		break;

	case UART_RX_DISABLED:
		/* Reception stopped. Restart it. */
		(void)uart_rx_enable(dev, receive_buffers[0],
				     sizeof(receive_buffers[0]), 100);
		break;

	case UART_TX_DONE:
	case UART_TX_ABORTED:
		/*
		 * THE point of this module. The final stop bit has left the
		 * shift register, so the bus is ours to release -- and we
		 * release it HERE, in interrupt context, rather than waking
		 * the sending thread and letting it do the job.
		 *
		 * Why: a node may answer the instant it sees the line idle.
		 * If DE stayed high until the scheduler got round to us, we
		 * would still be driving the bus while it replied, and two
		 * transmitters would be fighting over the same pair.
		 */
		gpio_pin_set_dt(&rs485_dir, 0);
		k_sem_give(&transmit_complete);
		break;

	default:
		break;
	}
}

void link_send(const uint8_t *raw, size_t raw_length)
{
	/*
	 * NOT a stack local any more. uart_tx() hands this address to the DMA
	 * and returns immediately; the hardware reads the bytes out over the
	 * following milliseconds. A buffer that died when the function
	 * returned would be transmitted as whatever landed there next.
	 *
	 * File scope is safe here only because one thread transmits. The real
	 * Hub has the same constraint and solves it the same way, with a
	 * single RS485 manager thread owning the buffer.
	 */
	static uint8_t encoded[rig::transport::max_encoded_uart_frame];

	const size_t encoded_length =
		rig::transport::cobs_encode(raw, raw_length, encoded, sizeof(encoded));
	if (encoded_length == 0U) {
		return;
	}

	k_sem_reset(&transmit_complete);

	/*
	 * Turnaround guard. The far end only stopped driving a moment ago and
	 * a transceiver takes time to let go. Driving too early collides with
	 * the tail of somebody else's frame. The real node waits 40 us here
	 * for exactly this reason.
	 */
	k_busy_wait(40);

	gpio_pin_set_dt(&rs485_dir, 1);
	k_busy_wait(1);    /* let DE settle before the first start bit */

	if (uart_tx(link, encoded, encoded_length, SYS_FOREVER_US) != 0) {
		gpio_pin_set_dt(&rs485_dir, 0);
		return;
	}

	/* Wait for UART_TX_DONE. The callback has already dropped DE by the
	 * time this returns -- we wait only so the next caller cannot
	 * overwrite the buffer while the DMA is still reading it. */
	if (k_sem_take(&transmit_complete, K_MSEC(20)) != 0) {
		(void)uart_tx_abort(link);
		(void)k_sem_take(&transmit_complete, K_MSEC(20));
		return;
	}
	++to_node;
}

/* ------------------------------------------------------------- BLE side --*/
/* The PC writes a whole Transport v1 frame here. We do not re-frame it: the
 * same bytes go out of the UART, COBS-wrapped. */
ssize_t rx_write(bt_conn *conn, const bt_gatt_attr *attr, const void *buf,
		 uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(flags);
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	rig::transport::FrameView frame;
	const auto result = rig::transport::decode_frame(
		static_cast<const uint8_t *>(buf), len, frame);
	if (result != rig::transport::DecodeResult::ok) {
		printk("BLE_RX bad frame (result=%u len=%u)\n",
		       static_cast<unsigned>(result), len);
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	printk("BLE->UART dst=0x%02x transfer=%u payload=%u\n",
	       frame.header.destination, frame.header.transfer_id,
	       static_cast<unsigned>(frame.payload_length));

	link_send(static_cast<const uint8_t *>(buf), len);
	return len;
}

void tx_ccc_changed(const bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	printk("notifications %s\n",
	       value == BT_GATT_CCC_NOTIFY ? "ENABLED" : "disabled");
}

BT_GATT_SERVICE_DEFINE(bridge_svc,
	BT_GATT_PRIMARY_SERVICE(&service_uuid),
	BT_GATT_CHARACTERISTIC(&rx_uuid.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, nullptr, rx_write, nullptr),
	BT_GATT_CHARACTERISTIC(&tx_uuid.uuid, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, nullptr, nullptr, nullptr),
	BT_GATT_CCC(tx_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* [0] svc, [1] rx decl, [2] rx value, [3] tx decl, [4] tx value, [5] CCC */
#define TX_ATTR (&bridge_svc.attrs[4])

void connected(bt_conn *conn, uint8_t err)
{
	if (err != 0) {
		printk("connection failed (0x%02x)\n", err);
		return;
	}
	current_conn = bt_conn_ref(conn);
	printk("connected\n");
}

void disconnected(bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	printk("disconnected (0x%02x)\n", reason);
	if (current_conn != nullptr) {
		bt_conn_unref(current_conn);
		current_conn = nullptr;
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

const bt_data advertising[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};
const bt_data scan_response[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BRIDGE_SERVICE_UUID),
};

/* -------------------------------------------- UART -> BLE, in a thread ---*/
void link_thread(void *, void *, void *)
{
	uint8_t byte;
	while (true) {
		k_msgq_get(&link_bytes, &byte, K_FOREVER);   /* blocks, costs nothing */

		rig::transport::FrameView frame;
		const auto result = decoder.push(byte, frame);
		if (result == rig::transport::DecodeResult::incomplete) {
			continue;
		}
		if (result != rig::transport::DecodeResult::ok) {
			++bad_frames;
			printk("UART bad frame (result=%u bad=%u)\n",
			       static_cast<unsigned>(result), bad_frames);
			continue;
		}

		++from_node;
		printk("UART->BLE src=0x%02x transfer=%u payload=%u\n",
		       frame.header.source, frame.header.transfer_id,
		       static_cast<unsigned>(frame.payload_length));

		/* Re-encode the decoded frame and notify it to the PC. */
		uint8_t raw[rig::transport::max_raw_frame_size]{};
		const size_t raw_length = rig::transport::encode_frame(
			frame.header, frame.payload, frame.payload_length,
			raw, sizeof(raw));
		if (raw_length != 0U && current_conn != nullptr) {
			(void)bt_gatt_notify(current_conn, TX_ATTR, raw, raw_length);
		}
	}
}
K_THREAD_DEFINE(link_thread_id, 2048, link_thread, nullptr, nullptr, nullptr,
		7, 0, 0);

} // namespace

int main(void)
{
	if (!device_is_ready(link)) {
		printk("ERROR: uart1 not ready\n");
		return -1;
	}
	if (!gpio_is_ready_dt(&rs485_dir)) {
		printk("ERROR: rs485 direction pin not ready\n");
		return -1;
	}
	/* Start out listening. An RS485 node's resting state is always
	 * receive -- one that powers up driving would jam the bus. */
	if (gpio_pin_configure_dt(&rs485_dir, GPIO_OUTPUT_INACTIVE) != 0) {
		printk("ERROR: rs485 direction pin configure failed\n");
		return -1;
	}

	if (uart_callback_set(link, link_event, nullptr) != 0) {
		printk("ERROR: uart_callback_set failed\n");
		return -1;
	}
	/* 100 us of idle line ends a receive chunk and raises UART_RX_RDY.
	 * At 460800 baud one byte is 21.7 us, so this fires a few byte-times
	 * after a frame ends rather than after every single byte. */
	if (uart_rx_enable(link, receive_buffers[0],
			   sizeof(receive_buffers[0]), 100) != 0) {
		printk("ERROR: uart_rx_enable failed\n");
		return -1;
	}

	if (bt_enable(nullptr) != 0) {
		printk("ERROR: bt_enable failed\n");
		return -1;
	}
	if (bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, advertising, ARRAY_SIZE(advertising),
			    scan_response, ARRAY_SIZE(scan_response)) != 0) {
		printk("ERROR: advertising failed\n");
		return -1;
	}

	printk("bridge advertising as \"%s\"\n", CONFIG_BT_DEVICE_NAME);
	printk("  RS485 460800, P0.27=DI P0.26=RO P1.12=DE -> node at 0x%02x\n",
	       rig::config::node_address(0));
	return 0;
}
