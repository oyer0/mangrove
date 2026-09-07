/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <mg/device_service.h>

/* Rebuild a copied, bounded view of currently known kernel devices. */
void device_snapshot_refresh(void);
mg_result_t device_snapshot_read(u32 filter, u64 device_id, u32 offset,
                                 u64 snapshot_generation,
                                 mg_device_info_t *output, u32 capacity,
                                 u32 *out_total, u64 *out_generation);
