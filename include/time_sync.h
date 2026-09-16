/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef TIME_SYNC_H__
#define TIME_SYNC_H__

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Feed one reference pair into the clock servo.
 *
 * Every valid SDU provides two readings of the same physical instant:
 * the broadcaster's timestamp (carried in the SDU) and the local
 * controller timestamp (the SDU synchronization reference).
 *
 * @param tx_ts_us    Broadcaster controller timestamp (32-bit, wraps).
 * @param local_ts_us Local controller timestamp of the same instant.
 */
void time_sync_update(uint32_t tx_ts_us, uint32_t local_ts_us);

/**
 * @brief Map a local 32-bit controller timestamp onto the broadcaster's
 *        timebase.
 *
 * Both clocks wrap at the same ~1 MHz rate, so the mapping is a wrap-safe
 * unsigned 32-bit addition; do not mix in the local 64-bit GRTC high bits
 * and do not sign-extend the 32-bit value: after ~35.8 min of uptime the
 * raw controller time passes 2^31 and a signed interpretation would go
 * negative.
 *
 * @param local_ts_us Local controller timestamp (32-bit, e.g. the ISO SDU
 *        synchronization reference).
 * @return The corresponding time in the broadcaster's timebase (us,
 *         unsigned 32-bit, same modulus as the TX timestamps).
 */
uint32_t time_sync_to_shared(uint32_t local_ts_us);

/**
 * @brief Whether the servo has collected enough samples and is tracking.
 */
bool time_sync_is_locked(void);

/** Servo statistics, sampled at the 10 s reporting cadence. */
struct time_sync_stats {
	bool locked;
	uint32_t samples;     /**< Accepted updates since boot. */
	uint32_t rejected;    /**< Samples rejected as outliers since boot. */
	int32_t offset_us;    /**< Estimated offset, integer part (us). */
	int32_t offset_frac;  /**< Offset hundredths of a us, 0..99. */
	int32_t drift_ppm;    /**< Estimated drift (us/s == ppm). */
	int32_t resid_max_us; /**< Worst |innovation| in the current window. */
};

/** Long-term stability summary, sampled at the LT reporting cadence. */
struct time_sync_lt_stats {
	uint32_t uptime_s;
	uint32_t samples;
	uint32_t rejected;
	bool locked;
	int32_t offset_us;
	int32_t offset_frac;
	int32_t drift_ppm;
	int32_t off_min_us;   /**< Min offset seen since the previous summary. */
	int32_t off_max_us;   /**< Max offset seen since the previous summary. */
	int32_t off_span_us;  /**< max - min, the drift/temperature excursion. */
	int32_t resid_max_us; /**< Worst |innovation| since the previous summary. */
	uint32_t window_updates;
};

/**
 * @brief Read the current servo statistics without changing filter state.
 */
void time_sync_stats_snapshot(struct time_sync_stats *out);

/**
 * @brief Read the long-term stability summary and start a new window.
 *
 * Safe to call from the Bluetooth RX workqueue; the reporting window is
 * reset so the next summary covers a fresh interval.
 */
void time_sync_lt_stats_snapshot(struct time_sync_lt_stats *out);

#endif /* TIME_SYNC_H__ */
