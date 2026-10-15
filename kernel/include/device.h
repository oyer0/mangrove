/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/device_service.h>

/* Sleepable inspection entry; caller holds no VFS/block/management locks.
 * Generation zero refreshes, otherwise copies only that completed generation.
 * Cancellation/admission failure returns BUSY; stale/inconsistent capture returns
 * RETRY. Errors leave output/count/generation unchanged. */
mg_result_t device_snapshot_read(u32 filter, u64 device_id, u32 offset,
                                 u64 snapshot_generation,
                                 mg_device_info_t *output, u32 capacity,
                                 u32 *out_total, u64 *out_generation);
