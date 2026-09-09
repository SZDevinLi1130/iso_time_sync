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
 * @brief Map a local GRTC time onto the broadcaster's timebase.
 *
 * @param local_grtc_us Local 64-bit GRTC time in microseconds.
 * @return The corresponding time in the broadcaster's timebase (us).
 */
int64_t time_sync_to_shared(uint64_t local_grtc_us);

/**
 * @brief Whether the servo has collected enough samples and is tracking.
 */
bool time_sync_is_locked(void);

/** @brief Print servo statistics (offset, drift, lock state). */
void time_sync_stats_print(void);

#endif /* TIME_SYNC_H__ */
