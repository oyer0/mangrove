#pragma once

#include <mg/device_service.h>

/* A single bounded snapshot contains whole disks and their partitions in the
 * same authoritative ordering used by the device service. */
#define MG_STORAGE_SNAPSHOT_MAX 128U

mg_result_t storage_snapshot_read(mg_device_info_t *devices, u32 capacity,
                                  u32 *out_count);
