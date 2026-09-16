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

/** @brief Print servo statistics (offset, drift, lock state). */
void time_sync_stats_print(void);

#endif /* TIME_SYNC_H__ */
