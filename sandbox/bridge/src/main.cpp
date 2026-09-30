/*
 * Sandbox "Korora": a BLE <-> UART bridge speaking real Transport v1.
 *
 *   PC --BLE--> THIS BOARD --USART1 460800--> fairy-lite --> LED
 *
 * The PC writes a complete Transport v1 frame to the RX characteristic.
 * This board decodes it, checks the destination, and forwards it down the
 * UART with COBS framing. Responses travel back the same way and are
 * notified to the PC.
 *
 * Framing uses firmware/shared, the same library the production firmware
 * and fairy-lite use -- so the bytes on both links are genuinely the real
 * protocol, not a lookalike.
 *
 * Wiring: DK P0.27 -> Nucleo PC5,  DK P0.26 <- Nucleo PC4,  GND <-> GND
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include "fairy_shared/system_config.hpp"
#include "fairy_shared/transport.hpp"

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
bt_conn *current_conn;

/* UART ISR -> thread handoff. A message queue rather than Fairy's hand-rolled
 * ring, because here there IS a scheduler to hand off to. */
K_MSGQ_DEFINE(link_bytes, sizeof(uint8_t), 1024, 1);

fairy::transport::UartDecoder decoder;
uint32_t to_fairy;
uint32_t from_fairy;
uint32_t bad_frames;

/* ------------------------------------------------------------ UART side --*/
void link_isr(const device *dev, void *)
{
	if (!uart_irq_update(dev) || !uart_irq_rx_ready(dev)) {
		return;
	}
	uint8_t byte;
	while (uart_fifo_read(dev, &byte, 1) == 1) {
		/* K_NO_WAIT: never block in an ISR. Full queue drops the byte,
		 * and the frame's CRC will reject the result. */
		(void)k_msgq_put(&link_bytes, &byte, K_NO_WAIT);
	}
}

void link_send(const uint8_t *raw, size_t raw_length)
{
	uint8_t encoded[fairy::transport::max_encoded_uart_frame]{};
	const size_t encoded_length =
		fairy::transport::cobs_encode(raw, raw_length, encoded, sizeof(encoded));
	if (encoded_length == 0U) {
		return;
	}
	for (size_t i = 0; i < encoded_length; ++i) {
		uart_poll_out(link, encoded[i]);
	}
	++to_fairy;
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

	fairy::transport::FrameView frame;
	const auto result = fairy::transport::decode_frame(
		static_cast<const uint8_t *>(buf), len, frame);
	if (result != fairy::transport::DecodeResult::ok) {
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

		fairy::transport::FrameView frame;
		const auto result = decoder.push(byte, frame);
		if (result == fairy::transport::DecodeResult::incomplete) {
			continue;
		}
		if (result != fairy::transport::DecodeResult::ok) {
			++bad_frames;
			printk("UART bad frame (result=%u bad=%u)\n",
			       static_cast<unsigned>(result), bad_frames);
			continue;
		}

		++from_fairy;
		printk("UART->BLE src=0x%02x transfer=%u payload=%u\n",
		       frame.header.source, frame.header.transfer_id,
		       static_cast<unsigned>(frame.payload_length));

		/* Re-encode the decoded frame and notify it to the PC. */
		uint8_t raw[fairy::transport::max_raw_frame_size]{};
		const size_t raw_length = fairy::transport::encode_frame(
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
	uart_irq_callback_user_data_set(link, link_isr, nullptr);
	uart_irq_rx_enable(link);

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
	printk("  UART1 460800, P0.27=TX P0.26=RX -> fairy-lite at 0x%02x\n",
	       fairy::config::fairy_address(0));
	return 0;
}
