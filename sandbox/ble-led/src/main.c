/*
 * Minimal BLE peripheral: a stripped-down Adelie <-> Korora link.
 *
 *   host writes 1 byte  ->  LED1 on/off        (the "command" direction)
 *   BUTTON1 pressed     ->  notify 1 byte      (the "record" direction)
 *
 * Real Korora wraps both directions in the Transport v1 framing (header,
 * CRC16, fragmentation). Here the payload IS the whole message, so you can
 * see the BLE plumbing without the protocol on top.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

/* ------------------------------------------------------------------ UUIDs --
 * A custom 128-bit service. These are just random numbers -- the only rule
 * is that both ends agree. Deliberately NOT Korora's a88279d0-... so the two
 * can never be confused on air.
 */
#define SANDBOX_SERVICE_UUID \
	BT_UUID_128_ENCODE(0x5b1e0001, 0xf00d, 0x4a5e, 0x9c33, 0x0a1b2c3d4e5f)
#define SANDBOX_LED_UUID \
	BT_UUID_128_ENCODE(0x5b1e0002, 0xf00d, 0x4a5e, 0x9c33, 0x0a1b2c3d4e5f)
#define SANDBOX_BUTTON_UUID \
	BT_UUID_128_ENCODE(0x5b1e0003, 0xf00d, 0x4a5e, 0x9c33, 0x0a1b2c3d4e5f)

static struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(SANDBOX_SERVICE_UUID);
static struct bt_uuid_128 led_uuid     = BT_UUID_INIT_128(SANDBOX_LED_UUID);
static struct bt_uuid_128 button_uuid  = BT_UUID_INIT_128(SANDBOX_BUTTON_UUID);

/* ------------------------------------------------------------- hardware --*/
static const struct gpio_dt_spec led    = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback button_cb;

static struct bt_conn *current_conn;
static uint8_t led_state;
static uint8_t button_presses;

/* --------------------------------------------------- GATT: LED (write) ----
 * Called in BLE stack context when the host writes to the characteristic.
 * Return the number of bytes consumed, or a negative BT_GATT_ERR.
 */
static ssize_t led_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 const void *buf, uint16_t len, uint16_t offset,
			 uint8_t flags)
{
	ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(flags);

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len != 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	led_state = ((const uint8_t *)buf)[0] ? 1 : 0;
	gpio_pin_set_dt(&led, led_state);
	printk("LED <- %u\n", led_state);
	return len;
}

static ssize_t led_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			void *buf, uint16_t len, uint16_t offset)
{
	ARG_UNUSED(conn); ARG_UNUSED(attr);
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &led_state, sizeof(led_state));
}

/* Called when the host subscribes/unsubscribes to button notifications. */
static void button_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	printk("notifications %s\n",
	       value == BT_GATT_CCC_NOTIFY ? "ENABLED" : "disabled");
}

/* ------------------------------------------------------ the GATT service --
 * Attribute order matters and is part of the protocol: each characteristic
 * is a declaration followed by its value, and a CCC descriptor if it
 * supports notifications.
 */
BT_GATT_SERVICE_DEFINE(sandbox_svc,
	BT_GATT_PRIMARY_SERVICE(&service_uuid),

	BT_GATT_CHARACTERISTIC(&led_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       led_read, led_write, NULL),

	BT_GATT_CHARACTERISTIC(&button_uuid.uuid,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),
	BT_GATT_CCC(button_ccc_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* The button value attribute is index 4 in the table above:
 * 0 service, 1 led decl, 2 led value, 3 button decl, 4 button value, 5 CCC */
#define BUTTON_ATTR (&sandbox_svc.attrs[4])

/* ------------------------------------------------------------- button ----
 * GPIO interrupt context. Notifying from here is allowed because
 * bt_gatt_notify() queues rather than blocks -- compare G5.
 */
static void button_pressed(const struct device *port, struct gpio_callback *cb,
			   gpio_port_pins_t pins)
{
	ARG_UNUSED(port); ARG_UNUSED(cb); ARG_UNUSED(pins);

	button_presses++;
	printk("BUTTON -> %u\n", button_presses);

	if (current_conn != NULL) {
		(void)bt_gatt_notify(current_conn, BUTTON_ATTR,
				     &button_presses, sizeof(button_presses));
	}
}

/* --------------------------------------------------------- connection ----*/
static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err != 0) {
		printk("connection failed (0x%02x)\n", err);
		return;
	}
	current_conn = bt_conn_ref(conn);
	printk("connected\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	printk("disconnected (0x%02x)\n", reason);
	if (current_conn != NULL) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	/* Outputs safe on disconnect -- the same rule the real system uses. */
	led_state = 0;
	gpio_pin_set_dt(&led, 0);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

/* ------------------------------------------------------------ advertising */
static const struct bt_data advertising[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static const struct bt_data scan_response[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, SANDBOX_SERVICE_UUID),
};

/* ------------------------------------------------------------------ main --*/
int main(void)
{
	int err;

	if (!gpio_is_ready_dt(&led) || !gpio_is_ready_dt(&button)) {
		printk("ERROR: GPIO not ready\n");
		return -1;
	}
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

	gpio_pin_configure_dt(&button, GPIO_INPUT);
	gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_init_callback(&button_cb, button_pressed, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb);

	err = bt_enable(NULL);
	if (err != 0) {
		printk("ERROR: bt_enable failed (%d)\n", err);
		return -1;
	}

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
			      advertising, ARRAY_SIZE(advertising),
			      scan_response, ARRAY_SIZE(scan_response));
	if (err != 0) {
		printk("ERROR: advertising failed (%d)\n", err);
		return -1;
	}

	printk("ble-led advertising as \"%s\"\n", CONFIG_BT_DEVICE_NAME);
	printk("  write 1 byte to the LED characteristic to toggle LED1\n");
	printk("  press BUTTON1 to notify the host\n");

	/* Nothing to do here -- everything happens in callbacks. */
	return 0;
}
