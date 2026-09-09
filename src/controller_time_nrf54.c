/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** This file implements controller time management for 54 Series devices
 *
 * Two GRTC compare channels are allocated: one generates the "set high"
 * event and one generates the "set low" event. This allows presenting an
 * absolute GPIO level at a controller-timed timestamp via DPPI/GPIOTE,
 * which is robust against lost SDUs (idempotent, self-correcting).
 */

#include <zephyr/kernel.h>
#include <nrfx_grtc.h>
#include "iso_time_sync.h"

static uint8_t grtc_channel_set;
static uint8_t grtc_channel_clr;

int controller_time_init(void)
{
	int ret;

	ret = nrfx_grtc_channel_alloc(&grtc_channel_set);
	if (ret < 0) {
		printk("Failed allocating GRTC set channel (ret: %d)\n", ret);
		return ret;
	}

	ret = nrfx_grtc_channel_alloc(&grtc_channel_clr);
	if (ret < 0) {
		printk("Failed allocating GRTC clr channel (ret: %d)\n", ret);
		return ret;
	}

	nrf_grtc_sys_counter_compare_event_enable(NRF_GRTC, grtc_channel_set);
	nrf_grtc_sys_counter_compare_event_enable(NRF_GRTC, grtc_channel_clr);

	return 0;
}

uint64_t controller_time_us_get(void)
{
	return nrfx_grtc_syscounter_get();
}

void controller_time_trigger_at(uint64_t timestamp_us, bool high_level)
{
	int ret;

	nrfx_grtc_channel_t chan_data = {
		.channel = high_level ? grtc_channel_set : grtc_channel_clr,
	};

	ret = nrfx_grtc_syscounter_cc_absolute_set(&chan_data, timestamp_us, false);
	if (ret != 0) {
		printk("Failed setting CC (ret: %d)\n", ret);
	}
}

uint32_t controller_time_trigger_event_addr_get(bool high_level)
{
	return nrf_grtc_event_address_get(NRF_GRTC,
					  nrf_grtc_sys_counter_compare_event_get(high_level ?
										 grtc_channel_set :
										 grtc_channel_clr));
}

SYS_INIT(controller_time_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
