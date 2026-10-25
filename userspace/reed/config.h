/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/types.h>
#include <mg/error.h>

#define REED_CONFIG_TAB_WIDTH_MAX 16U

typedef struct reed_config {
    bool wrap;
    u32 tab_width;
} reed_config_t;

void reed_config_defaults(reed_config_t *config);
bool reed_config_parse(const char *text, usize length,
                       reed_config_t *config, char *diagnostic,
                       usize diagnostic_capacity);
/* Missing configuration is success with defaults. This loader is read-only;
 * it neither creates directories nor writes a default file. */
mg_result_t reed_config_load(reed_config_t *config, char *diagnostic,
                             usize diagnostic_capacity);
