/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/error.h>
#include <mg/types.h>

/* The databases are presentation data.  A command keeps only the small set
 * of IDs present in its current kernel snapshot, never the whole database. */
#define MG_HARDWARE_ID_MAX_MATCHES 128U
#define MG_HARDWARE_ID_NAME_MAX    128U

typedef struct {
    u16 vendor_id;
    u16 device_id;
    bool vendor_found;
    bool device_found;
    char vendor[MG_HARDWARE_ID_NAME_MAX];
    char device[MG_HARDWARE_ID_NAME_MAX];
} mg_hardware_id_match_t;

typedef struct {
    mg_hardware_id_match_t entries[MG_HARDWARE_ID_MAX_MATCHES];
    usize count;
} mg_hardware_ids_t;

void hardware_ids_init(mg_hardware_ids_t *ids);
bool hardware_ids_add(mg_hardware_ids_t *ids, u16 vendor_id, u16 device_id);
mg_result_t hardware_ids_load(const char *path, mg_hardware_ids_t *ids);
const mg_hardware_id_match_t *hardware_ids_find(const mg_hardware_ids_t *ids,
                                                u16 vendor_id, u16 device_id);
