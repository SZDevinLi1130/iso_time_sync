/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @file time_sync.c
 *
 * Clock servo that maps the local GRTC timebase onto the broadcaster's
 * controller timebase.
 *
 * Each valid SDU gives a reference pair (tx_ts, local_ts): two readings of
 * the same physical instant in two different free-running clocks. The offset
 * between the two clocks (tx_ts - local_ts) is estimated robustly (median of
 * a sliding window), and the drift rate (ppm) is estimated from the slope of
 * that offset. Both timestamps wrap at the same ~1 MHz rate, so the 32-bit
 * signed difference is wrap-safe and needs no unwrapping.
 *
 * shared(local) = local + offset
 */

#include <zephyr/kernel.h>
#include <stdlib.h>
#include <string.h>
#include "time_sync.h"

#define TS_RING_SIZE         256
#define TS_MIN_LOCK_SAMPLES  64
#define TS_SDU_INTERVAL_US   5000UL    /* nominal SDU interval (see prj.conf) */

static int32_t ts_ring[TS_RING_SIZE];
static uint32_t ts_local_ring[TS_RING_SIZE]; /* local ts of each sample */
static uint16_t ts_count;
static uint16_t ts_head; /* index of the oldest sample */

/* Working buffers, file-scoped rather than on the call stack:
 * time_sync_update() runs in the BT RX workqueue whose stack is small.
 */
static int32_t ts_scratch[TS_RING_SIZE];
static int32_t ts_ordered[TS_RING_SIZE];

static bool ts_locked;
static int64_t ts_offset_us;  /* shared = local + offset (at ts_ref_local) */
static int64_t ts_drift_ppm;  /* drift in us/s (ppm) */
static uint32_t ts_ref_local; /* local ts at which ts_offset_us is valid */

static uint32_t ts_update_cnt;
static int32_t ts_residual_max_us;

static int cmp_int32(const void *a, const void *b)
{
	int32_t x = *(const int32_t *)a;
	int32_t y = *(const int32_t *)b;

	return (x > y) - (x < y);
}

static int32_t median_of(const int32_t *vals, uint16_t n, int32_t *scratch)
{
	if (n == 0) {
		return 0;
	}

	memcpy(scratch, vals, n * sizeof(int32_t));
	qsort(scratch, n, sizeof(int32_t), cmp_int32);

	return scratch[n / 2];
}

void time_sync_update(uint32_t tx_ts_us, uint32_t local_ts_us)
{
	/* Wrap-safe offset between two 1 MHz clocks. */
	int32_t meas = (int32_t)(tx_ts_us - local_ts_us);
	uint16_t start;
	uint16_t half;
	int32_t med_old;
	int32_t med_new;
	int64_t dt_us;
	int32_t resid;

	ts_ring[ts_head] = meas;
	ts_local_ring[ts_head] = local_ts_us;
	ts_head = (ts_head + 1) % TS_RING_SIZE;
	if (ts_count < TS_RING_SIZE) {
		ts_count++;
	}

	ts_update_cnt++;

	if (ts_count < TS_MIN_LOCK_SAMPLES) {
		return;
	}

	/* Copy the ring into chronological order (oldest first). */
	start = (ts_head + TS_RING_SIZE - ts_count) % TS_RING_SIZE;
	for (uint16_t i = 0; i < ts_count; i++) {
		ts_ordered[i] = ts_ring[(start + i) % TS_RING_SIZE];
	}

	/* Offset = median of the full window (robust against outliers). */
	ts_offset_us = median_of(ts_ordered, ts_count, ts_scratch);

	/* Drift from the slope between the two half-window medians. */
	half = ts_count / 2;
	med_old = median_of(ts_ordered, half, ts_scratch);
	med_new = median_of(ts_ordered + (ts_count - half), half, ts_scratch);
	dt_us = (int64_t)half * TS_SDU_INTERVAL_US;

	/* (med_new - med_old)/dt_us is a fraction; *1e6 -> ppm (us/s). */
	ts_drift_ppm = (int64_t)(med_new - med_old) * 1000000 / dt_us;
	ts_locked = true;

	/* The median value corresponds to the chronologically middle sample;
	 * remember its local timestamp so to_shared() can extrapolate the
	 * drift forward to the current instant.
	 */
	ts_ref_local = ts_local_ring[(start + ts_count / 2) % TS_RING_SIZE];

	resid = meas - (int32_t)ts_offset_us;
	if (resid < 0) {
		resid = -resid;
	}
	if (resid > ts_residual_max_us) {
		ts_residual_max_us = resid;
	}
}

int64_t time_sync_to_shared(uint32_t local_ts_us)
{
	int64_t offset = ts_offset_us;

	if (ts_locked) {
		/* Extrapolate the drift from the reference instant (window
		 * midpoint) to the current instant. This also covers holdover,
		 * since age keeps growing while no updates arrive.
		 */
		int32_t age_us = (int32_t)(local_ts_us - ts_ref_local);

		offset += (int64_t)ts_drift_ppm * age_us / 1000000;
	}

	/* Map the local 32-bit timestamp onto the broadcaster's 32-bit
	 * timebase (both wrap at the same ~1 MHz rate).
	 */
	return (int64_t)(int32_t)local_ts_us + offset;
}

bool time_sync_is_locked(void)
{
	return ts_locked;
}

void time_sync_stats_print(void)
{
	printk("time_sync: locked=%d offset=%lld us drift=%lld ppm samples=%u max_resid=%d us\n",
	       ts_locked, (long long)ts_offset_us, (long long)ts_drift_ppm,
	       ts_count, ts_residual_max_us);
}
