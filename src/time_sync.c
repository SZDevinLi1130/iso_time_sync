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
 * between the two clocks (tx_ts - local_ts) is estimated robustly, and the
 * drift rate (ppm) is estimated from the slope of that offset.
 *
 * Both timestamps wrap at the same ~1 MHz rate, so the 32-bit signed
 * difference is wrap-safe and needs no unwrapping.
 *
 * Sub-microsecond accuracy: the offset is estimated with a trimmed mean
 * (drop the outer 25% on each side and average the rest) instead of an
 * integer median. Averaging recovers the fractional part of the offset that
 * a single 1 us-resolution measurement cannot represent, and the drift is
 * carried as fixed-point so the extrapolation does not lose precision to
 * integer division. All internal state is 1/256 us fixed point (TS_FRAC).
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

/* Fixed-point scales.
 * offset is kept in 1/256 us (TS_FRAC_BITS); drift is kept as
 * (1/256 us) per (2^TS_DRIFT_Q_BITS us), so the extrapolation is a
 * 64-bit multiply + shift with no integer-division truncation.
 */
#define TS_FRAC_BITS         8
#define TS_FRAC              (1 << TS_FRAC_BITS)
#define TS_DRIFT_Q_BITS      12

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
static int64_t ts_offset_q;  /* offset in 1/256 us (valid at ts_ref_local) */
static int64_t ts_drift_q;   /* drift: (1/256 us) per (2^TS_DRIFT_Q_BITS us) */
static uint32_t ts_ref_local;

static uint32_t ts_update_cnt;
static int32_t ts_residual_max_us;

static int cmp_int32(const void *a, const void *b)
{
	int32_t x = *(const int32_t *)a;
	int32_t y = *(const int32_t *)b;

	return (x > y) - (x < y);
}

/* Trimmed mean (drop the outer 25% on each side) in 1/256 us fixed point.
 * 'vals' is in chronological order; 'scratch' is used for sorting.
 */
static int64_t trimmed_mean_q(const int32_t *vals, uint16_t n, int32_t *scratch)
{
	uint16_t trim = n / 4;
	uint16_t lo = trim;
	uint16_t hi = n - trim;
	int64_t sum = 0;

	if (hi <= lo) {
		hi = lo + 1;
	}

	memcpy(scratch, vals, n * sizeof(int32_t));
	qsort(scratch, n, sizeof(int32_t), cmp_int32);

	for (uint16_t i = lo; i < hi; i++) {
		sum += scratch[i];
	}

	return (sum * TS_FRAC + (int64_t)(hi - lo) / 2) / (hi - lo);
}

void time_sync_update(uint32_t tx_ts_us, uint32_t local_ts_us)
{
	/* Wrap-safe offset between two 1 MHz clocks. */
	int32_t meas = (int32_t)(tx_ts_us - local_ts_us);
	uint16_t start;
	uint16_t half;
	int64_t med_old_q;
	int64_t med_new_q;
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

	/* Offset = trimmed mean of the full window (robust + sub-us). */
	ts_offset_q = trimmed_mean_q(ts_ordered, ts_count, ts_scratch);

	/* Drift from the slope between the two half-window trimmed means. */
	half = ts_count / 2;
	med_old_q = trimmed_mean_q(ts_ordered, half, ts_scratch);
	med_new_q = trimmed_mean_q(ts_ordered + (ts_count - half), half, ts_scratch);
	dt_us = (int64_t)half * TS_SDU_INTERVAL_US;

	/* drift_q = (med_new - med_old) [1/256 us] / dt_us [us], scaled by
	 * 2^TS_DRIFT_Q_BITS so the extrapolation keeps full precision.
	 */
	ts_drift_q = ((med_new_q - med_old_q) << TS_DRIFT_Q_BITS) / dt_us;
	ts_locked = true;

	/* The trimmed mean corresponds to the chronological midpoint of the
	 * window; remember its local timestamp so to_shared() can extrapolate
	 * the drift forward to the current instant.
	 */
	ts_ref_local = ts_local_ring[(start + ts_count / 2) % TS_RING_SIZE];

	resid = meas - (int32_t)(ts_offset_q / TS_FRAC);
	if (resid < 0) {
		resid = -resid;
	}
	if (resid > ts_residual_max_us) {
		ts_residual_max_us = resid;
	}
}

uint32_t time_sync_to_shared(uint32_t local_ts_us)
{
	int64_t offset_q = ts_offset_q;

	if (ts_locked) {
		/* Extrapolate the drift from the reference instant (window
		 * midpoint) to the current instant. This also covers holdover,
		 * since age keeps growing while no updates arrive.
		 */
		int32_t age_us = (int32_t)(local_ts_us - ts_ref_local);

		offset_q += (ts_drift_q * age_us) >> TS_DRIFT_Q_BITS;
	}

	/* Wrap-safe unsigned 32-bit mapping: both clocks run at the same
	 * ~1 MHz rate, so the (possibly negative) offset is applied modulo
	 * 2^32. Keeping the result unsigned matches the TX-side prints even
	 * after the raw controller time passes 2^31 (~35.8 min), where a
	 * signed interpretation would go negative.
	 */
	int32_t offset_us = (int32_t)((offset_q + TS_FRAC / 2) / TS_FRAC);

	return local_ts_us + (uint32_t)offset_us;
}

bool time_sync_is_locked(void)
{
	return ts_locked;
}

void time_sync_stats_print(void)
{
	/* offset_q is in 1/256 us; drift_q * 1e6 >> (DRIFT_Q+FRAC) -> ppm. */
	printk("time_sync: locked=%d offset=%lld.%02llu us drift=%lld ppm samples=%u max_resid=%d us\n",
	       ts_locked,
	       (long long)(ts_offset_q / TS_FRAC),
	       (long long)(((ts_offset_q % TS_FRAC) * 100) / TS_FRAC),
	       (long long)((ts_drift_q * 1000000) >> (TS_DRIFT_Q_BITS + TS_FRAC_BITS)),
	       ts_count, ts_residual_max_us);
}
