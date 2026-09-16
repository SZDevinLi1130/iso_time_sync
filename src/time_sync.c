/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @file time_sync.c
 *
 * Three-state Kalman filter that maps the local GRTC timebase onto the
 * broadcaster's controller timebase.
 *
 * Each valid SDU gives a reference pair (tx_ts, local_ts): two readings of
 * the same physical instant in two free-running 1 MHz clocks. The filter
 * estimates
 *
 *     x = [ offset (us), drift (us/s), drift-rate (us/s^2) ]
 *
 * and maps local -> shared with a second-order extrapolation from the last
 * update:
 *
 *     shared(t) = t + offset + drift*age + 0.5*drift_rate*age^2
 *
 * Why a Kalman filter instead of a fixed window:
 *  - The measurement noise (1 us GRTC quantization plus path jitter) and the
 *    clock's slow drift/temperature wander are handled with an explicit noise
 *    model, so the filter balances noise averaging against tracking lag
 *    automatically instead of using a hard-coded window length.
 *  - The drift-rate state tracks temperature-induced frequency ramps and
 *    improves holdover: when the BIG is lost the mapping keeps running with
 *    the last offset/drift/drift-rate.
 *  - The innovation is gated (|innov| > KF_GATE_SIGMA * sqrt(S)), so packet
 *    loss / retransmission outliers are rejected before they reach the state.
 *
 * Both clocks wrap at the same ~1 MHz rate, so the offset is an unsigned
 * 32-bit quantity modulo 2^32. All offset arithmetic is wrap-safe: the state
 * is kept within [-2^31, 2^31) and the innovation is reduced modulo 2^32.
 *
 * All state is static (the filter runs in the Bluetooth RX workqueue whose
 * stack is small). Floating point is used for clarity; the build does not
 * enable the FPU, so this is soft-float, which is negligible at 200 Hz.
 *
 * The KF_* tuning constants below are initial estimates. Refine them with the
 * offline Allan/TDEV analysis described in BIS_TIME_SYNC_CONTEXT.md.
 */

#include <zephyr/kernel.h>
#include "time_sync.h"

/* ---- Filter tuning -------------------------------------------------------- */
#define KF_MEAS_VAR_US2    1.0        /* R: measurement noise variance (us^2)  */
#define KF_JERK_PSD        1e-7       /* q: jerk process-noise PSD             */
#define KF_P0_OFFSET_US2   1.0e4      /* initial offset variance (us^2)        */
#define KF_P0_DRIFT       (20.0 * 20.0) /* initial drift variance (us/s)^2     */
#define KF_P0_ACC          1.0e-2     /* initial drift-rate variance           */
#define KF_LOCK_SAMPLES    64U        /* updates before the mapping is trusted */
#define KF_MAX_DT_S        0.5        /* larger gap -> re-init the offset      */
#define KF_GATE_SIGMA      6.0        /* reject |innov| > 6 sigma              */
#define KF_REJECT_LIMIT_US 100000.0   /* hard reject beyond 100 ms             */
#define KF_MAX_EXTRAP_S    10.0       /* cap the holdover extrapolation span   */

#define KF_TWO32 4294967296.0
#define KF_TWO31 2147483648.0

static double kf_x[3];
static double kf_P[3][3];
static uint32_t kf_last_local;
static bool kf_initialized;
static uint32_t kf_updates;
static uint32_t kf_rejects;
static bool kf_locked;

/* Reporting window since the previous time_sync_lt_stats_snapshot(). */
static double ts_win_off_min;
static double ts_win_off_max;
static bool ts_win_has_sample;
static double ts_win_resid_max;
static uint32_t ts_win_start_updates;

/* Keep the offset within one 32-bit wrap of the raw controller time. */
static double wrap_offset(double v)
{
	if (v >= KF_TWO31) {
		v -= KF_TWO32;
	} else if (v < -KF_TWO31) {
		v += KF_TWO32;
	}
	return v;
}

static void kf_reset_covariance(void)
{
	kf_P[0][0] = KF_P0_OFFSET_US2;
	kf_P[0][1] = 0.0;
	kf_P[0][2] = 0.0;
	kf_P[1][0] = 0.0;
	kf_P[1][1] = KF_P0_DRIFT;
	kf_P[1][2] = 0.0;
	kf_P[2][0] = 0.0;
	kf_P[2][1] = 0.0;
	kf_P[2][2] = KF_P0_ACC;
}

/* x = F x; P = F P F^T + Q, with F = [[1, dt, dt^2/2], [0, 1, dt], [0, 0, 1]]
 * and Q the discrete white-noise-jerk model.
 */
static void kf_predict(double dt)
{
	double hh = 0.5 * dt * dt;
	double t00, t01, t02, t10, t11, t12, t20, t21, t22;
	double dt2, dt3, dt4, dt5, q;

	/* x = F x (drift-rate state is unchanged). */
	kf_x[0] = kf_x[0] + kf_x[1] * dt + kf_x[2] * hh;
	kf_x[1] = kf_x[1] + kf_x[2] * dt;

	/* T = F P. */
	t00 = kf_P[0][0] + dt * kf_P[1][0] + hh * kf_P[2][0];
	t01 = kf_P[0][1] + dt * kf_P[1][1] + hh * kf_P[2][1];
	t02 = kf_P[0][2] + dt * kf_P[1][2] + hh * kf_P[2][2];
	t10 = kf_P[1][0] + dt * kf_P[2][0];
	t11 = kf_P[1][1] + dt * kf_P[2][1];
	t12 = kf_P[1][2] + dt * kf_P[2][2];
	t20 = kf_P[2][0];
	t21 = kf_P[2][1];
	t22 = kf_P[2][2];

	/* P = T F^T. */
	kf_P[0][0] = t00 + dt * t01 + hh * t02;
	kf_P[0][1] = t01 + dt * t02;
	kf_P[0][2] = t02;
	kf_P[1][0] = t10 + dt * t11 + hh * t12;
	kf_P[1][1] = t11 + dt * t12;
	kf_P[1][2] = t12;
	kf_P[2][0] = t20 + dt * t21 + hh * t22;
	kf_P[2][1] = t21 + dt * t22;
	kf_P[2][2] = t22;

	/* P += Q. */
	dt2 = dt * dt;
	dt3 = dt2 * dt;
	dt4 = dt3 * dt;
	dt5 = dt4 * dt;
	q = KF_JERK_PSD;
	kf_P[0][0] += q * dt5 / 20.0;
	kf_P[0][1] += q * dt4 / 8.0;
	kf_P[0][2] += q * dt3 / 6.0;
	kf_P[1][0] += q * dt4 / 8.0;
	kf_P[1][1] += q * dt3 / 3.0;
	kf_P[1][2] += q * dt2 / 2.0;
	kf_P[2][0] += q * dt3 / 6.0;
	kf_P[2][1] += q * dt2 / 2.0;
	kf_P[2][2] += q * dt;
}

void time_sync_update(uint32_t tx_ts_us, uint32_t local_ts_us)
{
	/* Wrap-safe measurement: two readings of the same instant. */
	double z = (double)(int32_t)(tx_ts_us - local_ts_us);

	if (!kf_initialized) {
		kf_x[0] = z;
		kf_x[1] = 0.0;
		kf_x[2] = 0.0;
		kf_reset_covariance();
		kf_last_local = local_ts_us;
		kf_initialized = true;
		kf_updates = 1;
	} else {
		int32_t dts = (int32_t)(local_ts_us - kf_last_local);
		double dt = (double)dts * 1e-6;

		if (dt <= 0.0) {
			/* Duplicate or out-of-order sample: ignore. */
			return;
		}

		if (dt > KF_MAX_DT_S) {
			/* BIG lost or re-synced: restart the offset, keep the
			 * drift estimate and widen the covariance so the filter
			 * re-converges quickly.
			 */
			kf_x[0] = z;
			kf_P[0][0] += KF_P0_OFFSET_US2;
			kf_P[1][1] += KF_P0_DRIFT;
			kf_P[2][2] += KF_P0_ACC;
			kf_last_local = local_ts_us;
			kf_updates++;
			kf_locked = kf_updates >= KF_LOCK_SAMPLES;
		} else {
			double innov, S, K0, K1, K2, ares;
			int j;

			kf_predict(dt);

			innov = wrap_offset(z - kf_x[0]);
			S = kf_P[0][0] + KF_MEAS_VAR_US2;

			if (innov * innov > KF_GATE_SIGMA * KF_GATE_SIGMA * S ||
			    innov * innov > KF_REJECT_LIMIT_US * KF_REJECT_LIMIT_US) {
				kf_rejects++;
			} else {
				K0 = kf_P[0][0] / S;
				K1 = kf_P[1][0] / S;
				K2 = kf_P[2][0] / S;

				kf_x[0] += K0 * innov;
				kf_x[1] += K1 * innov;
				kf_x[2] += K2 * innov;

				/* P = (I - K H) P. */
				for (j = 0; j < 3; j++) {
					double p0j = kf_P[0][j];

					kf_P[0][j] -= K0 * p0j;
					kf_P[1][j] -= K1 * p0j;
					kf_P[2][j] -= K2 * p0j;
				}
				/* Re-symmetrize against numerical drift. */
				kf_P[0][1] = kf_P[1][0];
				kf_P[0][2] = kf_P[2][0];
				kf_P[1][2] = kf_P[2][1];

				ares = innov < 0.0 ? -innov : innov;
				if (ares > ts_win_resid_max) {
					ts_win_resid_max = ares;
				}
			}

			kf_x[0] = wrap_offset(kf_x[0]);
			kf_last_local = local_ts_us;
			kf_updates++;
			if (!kf_locked && kf_updates >= KF_LOCK_SAMPLES) {
				kf_locked = true;
			}
		}
	}

	/* Reporting window: offset excursion since the last LT snapshot. */
	if (!ts_win_has_sample) {
		ts_win_off_min = kf_x[0];
		ts_win_off_max = kf_x[0];
		ts_win_has_sample = true;
	} else {
		if (kf_x[0] < ts_win_off_min) {
			ts_win_off_min = kf_x[0];
		}
		if (kf_x[0] > ts_win_off_max) {
			ts_win_off_max = kf_x[0];
		}
	}
}

uint32_t time_sync_to_shared(uint32_t local_ts_us)
{
	int32_t age;
	double age_s, off;
	int32_t off_i;

	if (!kf_locked) {
		return local_ts_us;
	}

	/* Extrapolate from the last update (also covers holdover). */
	age = (int32_t)(local_ts_us - kf_last_local);
	age_s = (double)age * 1e-6;
	if (age_s > KF_MAX_EXTRAP_S) {
		age_s = KF_MAX_EXTRAP_S;
	} else if (age_s < -KF_MAX_EXTRAP_S) {
		age_s = -KF_MAX_EXTRAP_S;
	}

	off = kf_x[0] + kf_x[1] * age_s + 0.5 * kf_x[2] * age_s * age_s;
	off_i = (int32_t)(off >= 0.0 ? off + 0.5 : off - 0.5);

	/* Wrap-safe unsigned mapping, same modulus as the TX timestamps. */
	return local_ts_us + (uint32_t)off_i;
}

bool time_sync_is_locked(void)
{
	return kf_locked;
}

static void off_parts(double off, int32_t *int_us, int32_t *frac_100)
{
	double v = off * 100.0;
	long long r = (long long)(v >= 0.0 ? v + 0.5 : v - 0.5);
	int32_t f = (int32_t)(r % 100);

	*int_us = (int32_t)(r / 100);
	*frac_100 = f < 0 ? -f : f;
}

static int32_t round_us(double v)
{
	return (int32_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
}

void time_sync_stats_snapshot(struct time_sync_stats *out)
{
	out->locked = kf_locked;
	out->samples = kf_updates;
	out->rejected = kf_rejects;
	off_parts(kf_x[0], &out->offset_us, &out->offset_frac);
	out->drift_ppm = round_us(kf_x[1]);
	out->resid_max_us = round_us(ts_win_resid_max);
}

void time_sync_lt_stats_snapshot(struct time_sync_lt_stats *out)
{
	int32_t mn, mx;

	out->uptime_s = (uint32_t)(k_uptime_get() / 1000);
	out->samples = kf_updates;
	out->rejected = kf_rejects;
	out->locked = kf_locked;
	off_parts(kf_x[0], &out->offset_us, &out->offset_frac);
	out->drift_ppm = round_us(kf_x[1]);

	if (ts_win_has_sample) {
		mn = round_us(ts_win_off_min);
		mx = round_us(ts_win_off_max);
	} else {
		mn = round_us(kf_x[0]);
		mx = mn;
	}
	out->off_min_us = mn;
	out->off_max_us = mx;
	out->off_span_us = mx - mn;
	out->resid_max_us = round_us(ts_win_resid_max);
	out->window_updates = kf_updates - ts_win_start_updates;

	/* Start a fresh window. */
	ts_win_has_sample = false;
	ts_win_resid_max = 0.0;
	ts_win_start_updates = kf_updates;
}
