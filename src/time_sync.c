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
 * Sub-microsecond accuracy:
 *  - Offset: trimmed mean (drop the outer 25% on each side) in 1/256 us
 *    fixed point, recovering the fractional part a single 1 us measurement
 *    cannot represent.
 *  - Drift: least-squares linear regression over the in-band samples. A
 *    two-point slope estimator leaves a drift error of a few ppm, which the
 *    extrapolation from the window midpoint (~0.64 s) turns into a
 *    systematic ~1-2 us stamping error; the regression cuts that drift noise
 *    by roughly an order of magnitude.
 *
 * shared(local) = local + offset
 */

#include <zephyr/kernel.h>
#include <stdlib.h>
#include <string.h>
#include "time_sync.h"

/* Short window: the offset reference stays fresh (extrapolation span is
 * only half the window), which keeps the stamping residual caused by the
 * non-linear short-term wander of the two GRTC clocks well below 1 us.
 * Window 128 samples = 0.64 s at the 5 ms SDU interval.
 */
#define TS_RING_SIZE         128
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

/* Long-term verification window: min/max of the offset estimate and the worst
 * residual observed since the previous time_sync_lt_stats_print(). The offset
 * is kept in 1/256 us fixed point, the residual in us.
 */
static int64_t ts_win_off_min_q;
static int64_t ts_win_off_max_q;
static bool ts_win_has_sample;
static int32_t ts_win_resid_max_us;
static uint32_t ts_win_start_cnt;

static int cmp_int32(const void *a, const void *b)
{
	int32_t x = *(const int32_t *)a;
	int32_t y = *(const int32_t *)b;

	return (x > y) - (x < y);
}

void time_sync_update(uint32_t tx_ts_us, uint32_t local_ts_us)
{
	/* Wrap-safe offset between two 1 MHz clocks. */
	int32_t meas = (int32_t)(tx_ts_us - local_ts_us);
	uint16_t start;
	uint16_t trim;
	int32_t v_lo;
	int32_t v_hi;
	int64_t sum;
	uint16_t lo;
	uint16_t hi;
	int64_t sx;
	int64_t sy;
	int64_t sxx;
	int64_t sxy;
	uint16_t m;
	int64_t den;
	int64_t num;
	int64_t b_q;
	int64_t b_rem;
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

	/* Sorted copy, used for the percentile band. */
	memcpy(ts_scratch, ts_ordered, ts_count * sizeof(int32_t));
	qsort(ts_scratch, ts_count, sizeof(int32_t), cmp_int32);

	/* Offset = trimmed mean of the full window (robust + sub-us). */
	trim = ts_count / 4;
	lo = trim;
	hi = ts_count - trim;
	if (hi <= lo) {
		hi = lo + 1;
	}
	sum = 0;
	for (uint16_t i = lo; i < hi; i++) {
		sum += ts_scratch[i];
	}
	ts_offset_q = (sum * TS_FRAC + (int64_t)(hi - lo) / 2) / (hi - lo);

	/* Drift = least-squares slope over the in-band samples.
	 * y = meas [us], x = window index (1 sample = SDU interval).
	 */
	v_lo = ts_scratch[lo];
	v_hi = ts_scratch[hi - 1];
	sx = 0;
	sy = 0;
	sxx = 0;
	sxy = 0;
	m = 0;
	for (uint16_t i = 0; i < ts_count; i++) {
		if (ts_ordered[i] < v_lo || ts_ordered[i] > v_hi) {
			continue;
		}
		sx += i;
		sy += ts_ordered[i];
		sxx += (int64_t)i * i;
		sxy += (int64_t)i * ts_ordered[i];
		m++;
	}

	if (m >= 8) {
		den = (int64_t)m * sxx - (int64_t)sx * sx;
		if (den != 0) {
			/* Slope in us per sample, kept in 1/256-us fixed point
			 * to avoid any precision loss before the rescale.
			 */
			num = (int64_t)m * sxy - (int64_t)sx * sy;
			b_q = num / den;
			b_rem = num % den;

			/* drift_q = b [us/sample] * TS_FRAC * 2^TS_DRIFT_Q_BITS
			 *           / TS_SDU_INTERVAL_US
			 * Split quotient/remainder to keep intermediates small.
			 */
			ts_drift_q =
				(b_q * ((int64_t)TS_FRAC << TS_DRIFT_Q_BITS)) /
				(int64_t)TS_SDU_INTERVAL_US +
				(b_rem * ((int64_t)TS_FRAC << TS_DRIFT_Q_BITS)) /
				((int64_t)den * (int64_t)TS_SDU_INTERVAL_US);
		}
	}

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

	/* Track the offset excursion and worst residual for the long-term
	 * stability summary.
	 */
	if (!ts_win_has_sample) {
		ts_win_off_min_q = ts_offset_q;
		ts_win_off_max_q = ts_offset_q;
		ts_win_has_sample = true;
	} else {
		if (ts_offset_q < ts_win_off_min_q) {
			ts_win_off_min_q = ts_offset_q;
		}
		if (ts_offset_q > ts_win_off_max_q) {
			ts_win_off_max_q = ts_offset_q;
		}
	}
	if (resid > ts_win_resid_max_us) {
		ts_win_resid_max_us = resid;
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
	int64_t off_int = ts_offset_q / TS_FRAC;
	int64_t off_rem = ts_offset_q % TS_FRAC;

	if (off_rem < 0) {
		off_rem = -off_rem;
	}

	/* offset_q is in 1/256 us; drift_q * 1e6 >> (DRIFT_Q+FRAC) -> ppm. */
	printk("time_sync: locked=%d offset=%lld.%02lld us drift=%lld ppm samples=%u max_resid=%d us\n",
	       ts_locked,
	       (long long)off_int,
	       (long long)(off_rem * 100 / TS_FRAC),
	       (long long)((ts_drift_q * 1000000) >> (TS_DRIFT_Q_BITS + TS_FRAC_BITS)),
	       ts_count, ts_residual_max_us);
}

void time_sync_lt_stats_print(void)
{
	uint32_t uptime_s = (uint32_t)(k_uptime_get() / 1000);
	int64_t off_int = ts_offset_q / TS_FRAC;
	int64_t off_rem = ts_offset_q % TS_FRAC;
	int64_t off_min_q = ts_win_has_sample ? ts_win_off_min_q : ts_offset_q;
	int64_t off_max_q = ts_win_has_sample ? ts_win_off_max_q : ts_offset_q;
	uint32_t window_updates = ts_update_cnt - ts_win_start_cnt;

	if (off_rem < 0) {
		off_rem = -off_rem;
	}

	/* One line per reporting window for long-run (>= 1 hour) verification:
	 * absolute offset/drift, the offset excursion (min/max/span) seen in the
	 * window, and the worst residual. off_span growing over time is the
	 * signature of uncompensated drift or temperature wander.
	 */
	printk("time_sync_lt: t=%us samples=%u locked=%d offset=%lld.%02lld us "
	       "drift=%lld ppm off_min=%lld us off_max=%lld us off_span=%lld us "
	       "max_resid=%d us win_updates=%u\n",
	       uptime_s, ts_count, ts_locked,
	       (long long)off_int,
	       (long long)(off_rem * 100 / TS_FRAC),
	       (long long)((ts_drift_q * 1000000) >> (TS_DRIFT_Q_BITS + TS_FRAC_BITS)),
	       (long long)(off_min_q / TS_FRAC),
	       (long long)(off_max_q / TS_FRAC),
	       (long long)((off_max_q - off_min_q) / TS_FRAC),
	       ts_win_resid_max_us,
	       window_updates);

	/* Start a fresh window. */
	ts_win_has_sample = false;
	ts_win_resid_max_us = 0;
	ts_win_start_cnt = ts_update_cnt;
}
