/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/line_editor.h>
#include "config.h"
#include <mg/filesystem.h>

#define SHOOT_HISTORY_MAX_ENTRIES 4096U
#define SHOOT_HISTORY_ENTRY_CAPACITY 512U
#define SHOOT_HISTORY_PATH_CAPACITY 320U

typedef struct shoot_history_entry {
    bool pending;
    u64 saved_identifier;
    usize saved_offset;
} shoot_history_entry_t;

typedef struct shoot_history {
    mg_line_history_t editor_history;
    char *storage;
    char *draft;
    char directory[SHOOT_HISTORY_PATH_CAPACITY];
    char path[SHOOT_HISTORY_PATH_CAPACITY];
    shoot_config_t config;
    bool available;
    shoot_history_entry_t *entries;
    char *baseline;
    mg_file_lines_state_t baseline_state;
    bool baseline_valid;
    bool write_uncertain;
} shoot_history_t;

/* History is convenience state: allocation or filesystem failures are nonfatal. */
bool shoot_history_init(shoot_history_t *history, const shoot_config_t *config);
void shoot_history_destroy(shoot_history_t *history);
bool shoot_history_save(shoot_history_t *history);
void shoot_history_apply_config(shoot_history_t *history,
                                const shoot_config_t *config);
void shoot_history_record(shoot_history_t *history, const char *line);
void shoot_history_clear(shoot_history_t *history);
mg_result_t shoot_history_append(shoot_history_t *history);
mg_result_t shoot_history_write(shoot_history_t *history);
mg_result_t shoot_history_read(shoot_history_t *history, bool only_new);
bool shoot_history_delete(shoot_history_t *history, usize number);
mg_line_history_t *shoot_history_editor(shoot_history_t *history);
