/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef SYNC_LOG_H__
#define SYNC_LOG_H__

#include <stdint.h>
#include <stdbool.h>
#include "time_sync.h"

/**
 * @file sync_log.h
 *
 * Deferred UART logging for the ISO time sync demo.
 *
 * The periodic and event logs are formatted and printed by a dedicated
 * low-priority thread instead of the Bluetooth RX/TX workqueues. The workqueue
 * callbacks only enqueue a small fixed-size record, so console/printk latency
 * can never delay the controller-timed presentation path.
 *
 * The queue is non-blocking: if it is full the record is dropped rather than
 * stalling the caller.
 */

void sync_log_event(uint32_t counter, uint32_t timestamp_us);

void sync_log_sdu_rx(uint32_t counter, uint32_t ts, int btn_val, uint32_t shared_ts);

void sync_log_sdu_tx(uint32_t counter, uint32_t ts, uint32_t controller_time_us,
		     int btn_val, int time_to_trigger_us);

void sync_log_stats(const struct time_sync_stats *stats);

void sync_log_lt(const struct time_sync_lt_stats *lt, uint32_t received,
		 uint32_t lost, uint32_t resync);

#endif /* SYNC_LOG_H__ */
