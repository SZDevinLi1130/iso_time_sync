/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @file sync_log.c
 *
 * Deferred UART logging. The Bluetooth workqueue callbacks enqueue numeric
 * records into a message queue; a low-priority thread drains it and formats
 * the text. This keeps printk formatting and console latency out of the
 * time-critical ISO send/receive path.
 */

#include <zephyr/kernel.h>
#include "sync_log.h"

enum sync_log_kind {
	SYNC_LOG_SDU_RX,
	SYNC_LOG_SDU_TX,
	SYNC_LOG_EVENT,
	SYNC_LOG_STATS,
	SYNC_LOG_LT,
};

struct sync_log_item {
	uint8_t kind;
	uint32_t counter;
	uint32_t ts;
	uint32_t shared_ts;
	uint32_t controller_time_us;
	int btn_val;
	int time_to_trigger_us;
	uint32_t received;
	uint32_t lost;
	uint32_t resync;
	union {
		struct time_sync_stats stats;
		struct time_sync_lt_stats lt;
	} u;
};

K_MSGQ_DEFINE(sync_log_msgq, sizeof(struct sync_log_item), 8, 4);

static void sync_log_thread(void *a, void *b, void *c)
{
	struct sync_log_item it;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		if (k_msgq_get(&sync_log_msgq, &it, K_FOREVER) != 0) {
			continue;
		}

		switch (it.kind) {
		case SYNC_LOG_EVENT:
			printk("\x1B[1;31mSYNC EVENT: counter %u, timestamp %u us\x1B[0m\n",
			       it.counter, it.ts);
			break;
		case SYNC_LOG_SDU_RX:
			printk("Recv SDU counter %u timestamp %u us btn_val: %d shared_ts %u us\n",
			       it.counter, it.ts, it.btn_val, it.shared_ts);
			break;
		case SYNC_LOG_SDU_TX:
			printk("Sent SDU counter %u with timestamp %u us, controller_time %u us, "
			       "btn_val: %d LED will be set in %d us\n",
			       it.counter, it.ts, it.controller_time_us, it.btn_val,
			       it.time_to_trigger_us);
			break;
		case SYNC_LOG_STATS:
			printk("time_sync: locked=%d offset=%d.%02d us drift=%d ppm "
			       "samples=%u rejected=%u max_resid=%d us\n",
			       it.u.stats.locked, it.u.stats.offset_us,
			       it.u.stats.offset_frac, it.u.stats.drift_ppm,
			       it.u.stats.samples, it.u.stats.rejected,
			       it.u.stats.resid_max_us);
			break;
		case SYNC_LOG_LT: {
			int32_t tf = it.u.lt.temp_c_x100 % 100;
			int32_t rf = it.u.lt.temp_rate_x1000 % 1000;

			if (tf < 0) {
				tf = -tf;
			}
			if (rf < 0) {
				rf = -rf;
			}

			printk("time_sync_lt: t=%us samples=%u rejected=%u locked=%d "
			       "offset=%d.%02d us drift=%d ppm off_min=%d us off_max=%d us "
			       "off_span=%d us max_resid=%d us win_updates=%u\n",
			       it.u.lt.uptime_s, it.u.lt.samples, it.u.lt.rejected,
			       it.u.lt.locked, it.u.lt.offset_us, it.u.lt.offset_frac,
			       it.u.lt.drift_ppm, it.u.lt.off_min_us, it.u.lt.off_max_us,
			       it.u.lt.off_span_us, it.u.lt.resid_max_us,
			       it.u.lt.window_updates);
			if (it.u.lt.temp_valid) {
				printk("time_sync_temp: t=%us temp=%d.%02d C "
				       "rate=%d.%03d C/s\n",
				       it.u.lt.uptime_s, it.u.lt.temp_c_x100 / 100, tf,
				       it.u.lt.temp_rate_x1000 / 1000, rf);
			}
			printk("iso_rx_lt: t=%us received=%u lost=%u resync=%u\n",
			       it.u.lt.uptime_s, it.received, it.lost, it.resync);
			break;
		}
		default:
			break;
		}
	}
}

K_THREAD_DEFINE(sync_log_tid, 1536, sync_log_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

static void sync_log_put(const struct sync_log_item *item)
{
	/* Never block the caller (Bluetooth workqueue): drop when full. */
	(void)k_msgq_put(&sync_log_msgq, item, K_NO_WAIT);
}

void sync_log_event(uint32_t counter, uint32_t timestamp_us)
{
	struct sync_log_item it = {
		.kind = SYNC_LOG_EVENT,
		.counter = counter,
		.ts = timestamp_us,
	};

	sync_log_put(&it);
}

void sync_log_sdu_rx(uint32_t counter, uint32_t ts, int btn_val, uint32_t shared_ts)
{
	struct sync_log_item it = {
		.kind = SYNC_LOG_SDU_RX,
		.counter = counter,
		.ts = ts,
		.btn_val = btn_val,
		.shared_ts = shared_ts,
	};

	sync_log_put(&it);
}

void sync_log_sdu_tx(uint32_t counter, uint32_t ts, uint32_t controller_time_us,
		     int btn_val, int time_to_trigger_us)
{
	struct sync_log_item it = {
		.kind = SYNC_LOG_SDU_TX,
		.counter = counter,
		.ts = ts,
		.controller_time_us = controller_time_us,
		.btn_val = btn_val,
		.time_to_trigger_us = time_to_trigger_us,
	};

	sync_log_put(&it);
}

void sync_log_stats(const struct time_sync_stats *stats)
{
	struct sync_log_item it = {
		.kind = SYNC_LOG_STATS,
		.u.stats = *stats,
	};

	sync_log_put(&it);
}

void sync_log_lt(const struct time_sync_lt_stats *lt, uint32_t received,
		 uint32_t lost, uint32_t resync)
{
	struct sync_log_item it = {
		.kind = SYNC_LOG_LT,
		.received = received,
		.lost = lost,
		.resync = resync,
		.u.lt = *lt,
	};

	sync_log_put(&it);
}
