/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include "iso_time_sync.h"

#define BIS_NODE_COUNT 8U

/* Default BIS parameters used when the role is selected by GPIO instead of
 * the serial console.
 */
#define BIS_TX_RTN                       1
#define BIS_TX_MAX_TRANSPORT_LATENCY_MS  10
#define BIS_RX_DEFAULT_INDEX             1

/* Role selection is done by sampling P1.09 (Button 1 on the nRF54L15DK,
 * devicetree alias sw1) once at boot:
 *   - low level (button held / pin tied to GND) -> BIS transmitter
 *   - otherwise (pull-up keeps it high)         -> BIS receiver
 */
#define ROLE_PIN_SETTLE_TIME_MS 50
#define ROLE_PIN_SAMPLE_COUNT   5

static const struct gpio_dt_spec role_pin = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);

/* Button (sw0 = P1.13) that requests an immediate time-sync event on the
 * transmitter. Debounced in software.
 */
#define SYNC_BTN_DEBOUNCE_MS 50
static const struct gpio_dt_spec sync_btn = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback sync_btn_cb;
static int64_t last_sync_req_ms;

/** Sample the role pin and return true if the device shall act as BIS transmitter. */
static bool role_pin_selects_transmitter(void)
{
	int err;
	int low_count = 0;

	if (!gpio_is_ready_dt(&role_pin)) {
		printk("Role pin device not ready, defaulting to BIS receiver\n");
		return false;
	}

	err = gpio_pin_configure_dt(&role_pin, GPIO_INPUT);
	if (err != 0) {
		printk("Error %d: failed to configure role pin, defaulting to BIS receiver\n",
		       err);
		return false;
	}

	/* Let the pull-up settle, then sample the pin a few times to avoid
	 * deciding on a glitch.
	 */
	k_msleep(ROLE_PIN_SETTLE_TIME_MS);

	for (int i = 0; i < ROLE_PIN_SAMPLE_COUNT; i++) {
		int val = gpio_pin_get_dt(&role_pin);

		if (val < 0) {
			printk("Error %d: failed to read role pin, defaulting to BIS receiver\n",
			       val);
			return false;
		}

		/* The pin is GPIO_ACTIVE_LOW, so a logical 1 means the physical
		 * level is low.
		 */
		if (val > 0) {
			low_count++;
		}

		k_msleep(2);
	}

	return low_count > (ROLE_PIN_SAMPLE_COUNT / 2);
}

static void sync_btn_pressed(const struct device *dev, struct gpio_callback *cb,
			     uint32_t pins)
{
	int64_t now = k_uptime_get();

	(void)dev;
	(void)cb;
	(void)pins;

	if ((now - last_sync_req_ms) < SYNC_BTN_DEBOUNCE_MS) {
		return;
	}
	last_sync_req_ms = now;

	iso_tx_request_sync();
}

static void sync_button_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&sync_btn)) {
		printk("Sync button device not ready\n");
		return;
	}

	err = gpio_pin_configure_dt(&sync_btn, GPIO_INPUT);
	if (err != 0) {
		printk("Error %d: failed to configure sync button\n", err);
		return;
	}

	err = gpio_pin_interrupt_configure_dt(&sync_btn, GPIO_INT_EDGE_TO_ACTIVE);
	if (err != 0) {
		printk("Error %d: failed to configure sync button interrupt\n", err);
		return;
	}

	gpio_init_callback(&sync_btn_cb, sync_btn_pressed, BIT(sync_btn.pin));
	gpio_add_callback(sync_btn.port, &sync_btn_cb);

	printk("Sync button ready (sw0 = P1.13)\n");
}

int main(void)
{
	int err;
	bool transmitter;

	printk("Bluetooth ISO Time Sync Demo\n");

	transmitter = role_pin_selects_transmitter();
	printk("Role pin P1.09 %s -> %s role\n",
	       transmitter ? "low" : "not low",
	       transmitter ? "BIS transmitter" : "BIS receiver");

	err = timed_led_toggle_init();
	if (err != 0) {
		printk("Error failed to init LED device for toggling\n");
		return err;
	}

	/* Initialize the Bluetooth Subsystem */
	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	if (transmitter) {
		printk("Starting BIS transmitter with %u BIS streams, RTN: %d, max transport "
		       "latency %d ms\n",
		       BIS_NODE_COUNT, BIS_TX_RTN, BIS_TX_MAX_TRANSPORT_LATENCY_MS);
		sync_button_init();
		bis_transmitter_start(BIS_TX_RTN, BIS_TX_MAX_TRANSPORT_LATENCY_MS);
	} else {
		printk("Starting BIS receiver, BIS index %d\n", BIS_RX_DEFAULT_INDEX);
		bis_receiver_start(BIS_RX_DEFAULT_INDEX);
	}

	return 0;
}
