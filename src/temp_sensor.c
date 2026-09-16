/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @file temp_sensor.c
 *
 * Samples the on-chip temperature sensor periodically and feeds it to the
 * clock servo (time_sync_set_temperature()), which scales its process noise
 * with the temperature rate of change.
 *
 * A dedicated low-priority thread is used so the (blocking) sensor fetch never
 * runs in the Bluetooth workqueues.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include "time_sync.h"

static void temp_sensor_thread(void *a, void *b, void *c)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(temp));
	struct sensor_value val;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (!device_is_ready(dev)) {
		k_msleep(1000);
	}

	while (true) {
		if (sensor_sample_fetch(dev) == 0 &&
		    sensor_channel_get(dev, SENSOR_CHAN_DIE_TEMP, &val) == 0) {
			time_sync_set_temperature(sensor_value_to_double(&val));
		}
		k_msleep(CONFIG_TIME_SYNC_TEMP_PERIOD_MS);
	}
}

K_THREAD_DEFINE(temp_sensor_tid, 1024, temp_sensor_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
