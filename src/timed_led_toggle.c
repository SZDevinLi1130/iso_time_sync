/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** This file implements timed presentation of a trigger level on a GPIO.
 *
 * The GPIO (P1.10, devicetree alias led1 on the nRF54L15DK) is driven by
 * GPIOTE SET/CLR tasks that are linked over DPPI to two GRTC compare
 * events. Calling timed_led_toggle_trigger_at() arms the compare channel
 * matching the requested level, so the pin assumes the absolute level at
 * the requested controller timestamp. Presenting an absolute level (rather
 * than toggling) is idempotent: repeated or lost SDUs cannot invert the
 * pin state.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <soc.h>
#include <nrfx_gpiote.h>
#include <gpiote_nrfx.h>
#include <helpers/nrfx_gppi.h>
#include "iso_time_sync.h"

#define GPIOTE_NODE NRF_DT_GPIOTE_NODE(DT_ALIAS(led1), gpios)
#define LED_PIN NRF_DT_GPIOS_TO_PSEL(DT_ALIAS(led1), gpios)

/* Width of the pulse presented on the pin for a trigger event, in
 * microseconds (GRTC resolution is 1 us).
 */
#define TRIGGER_PULSE_WIDTH_US 1

static struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led1), gpios, {0});

int timed_led_toggle_init(void)
{
	int err;
	nrfx_gppi_handle_t ppi_led_set;
	nrfx_gppi_handle_t ppi_led_clr;
	uint8_t gpiote_chan_led;
	nrfx_gpiote_t *gpiote = &GPIOTE_NRFX_INST_BY_NODE(GPIOTE_NODE);

	const nrfx_gpiote_output_config_t gpiote_output_cfg = NRFX_GPIOTE_DEFAULT_OUTPUT_CONFIG;

	err = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (err != 0) {
		printk("Error %d: failed to configure LED device %s pin %d\n", err, led.port->name,
		       led.pin);
		return err;
	}

	if (nrfx_gpiote_channel_alloc(gpiote, &gpiote_chan_led) != 0) {
		printk("Failed allocating GPIOTE chan for setting led\n");
		return -ENOMEM;
	}

	const nrfx_gpiote_task_config_t task_cfg_led = {
		.task_ch = gpiote_chan_led,
		.polarity = NRF_GPIOTE_POLARITY_TOGGLE,
		.init_val = (led.dt_flags & GPIO_ACTIVE_LOW) ?
			NRF_GPIOTE_INITIAL_VALUE_HIGH : NRF_GPIOTE_INITIAL_VALUE_LOW,
	};

	if (nrfx_gpiote_output_configure(gpiote, LED_PIN, &gpiote_output_cfg,
					 &task_cfg_led) != 0) {
		printk("Failed configuring GPIOTE chan for setting led\n");
		return -ENOMEM;
	}

	/* GRTC set-event -> GPIOTE SET task (pin high at timestamp). */
	err = nrfx_gppi_conn_alloc(controller_time_trigger_event_addr_get(true),
				   nrfx_gpiote_set_task_address_get(gpiote, LED_PIN),
				   &ppi_led_set);
	if (err < 0) {
		printk("Failed allocating PPI chan for setting led high\n");
		return -ENOMEM;
	}

	/* GRTC clr-event -> GPIOTE CLR task (pin low at timestamp). */
	err = nrfx_gppi_conn_alloc(controller_time_trigger_event_addr_get(false),
				   nrfx_gpiote_clr_task_address_get(gpiote, LED_PIN),
				   &ppi_led_clr);
	if (err < 0) {
		printk("Failed allocating PPI chan for setting led low\n");
		return -ENOMEM;
	}

	nrfx_gppi_conn_enable(ppi_led_set);
	nrfx_gppi_conn_enable(ppi_led_clr);
	nrfx_gpiote_out_task_enable(gpiote, LED_PIN);

	return 0;
}

void timed_led_toggle_trigger_at(uint8_t value, uint32_t timestamp_us)
{
	/* First obtain the full 64-bit time. */
	const uint64_t current_time_us = controller_time_us_get();
	const uint64_t current_time_most_significant_word = current_time_us & 0xFFFFFFFF00000000UL;

	uint64_t full_timestamp_us = current_time_most_significant_word | timestamp_us;

	if (timestamp_us < (current_time_us & UINT32_MAX)) {
		/* Trigger time is after UINT32 wrap */
		full_timestamp_us += 0x100000000UL;
	}

	if (value != 0) {
		/* Present a narrow pulse: set the pin high at the presentation
		 * timestamp and back low TRIGGER_PULSE_WIDTH_US later.
		 */
		controller_time_trigger_at(full_timestamp_us, true);
		controller_time_trigger_at(full_timestamp_us + TRIGGER_PULSE_WIDTH_US, false);
	} else {
		/* Idle level is low. Re-arming CLR is idempotent and heals
		 * any pin state mismatch after lost SDUs.
		 */
		controller_time_trigger_at(full_timestamp_us, false);
	}
}
