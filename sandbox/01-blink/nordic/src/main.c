/*
 * Blink LED1 on the nRF52840 DK, and print each toggle to the console.
 *
 * Nothing here names a pin. "led0" is a devicetree alias the DK's board
 * definition provides; GPIO_DT_SPEC_GET resolves the port, pin number and
 * active-high/low flag at BUILD time.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#define BLINK_INTERVAL_MS 500

/* DT_ALIAS(led0) -> the board's first user LED. */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

int main(void)
{
	if (!gpio_is_ready_dt(&led)) {
		printk("ERROR: LED device not ready\n");
		return -1;
	}

	/* INACTIVE means "logically off" -- the driver works out whether that
	 * is electrically high or low from the devicetree flags. */
	if (gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) != 0) {
		printk("ERROR: could not configure LED\n");
		return -1;
	}

	printk("blink-nordic running: %s pin %d, every %d ms\n",
	       led.port->name, led.pin, BLINK_INTERVAL_MS);

	uint32_t count = 0;
	while (1) {
		gpio_pin_toggle_dt(&led);
		printk("toggle %u\n", count++);

		/* Blocks THIS thread only -- the CPU idles or runs others. */
		k_msleep(BLINK_INTERVAL_MS);
	}
	return 0;
}
