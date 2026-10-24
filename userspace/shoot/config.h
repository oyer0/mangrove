/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/terminal.h>

typedef struct shoot_config {
    mg_terminal_color_t prompt_color;
    mg_terminal_color_t completion_color;
    bool completion_case_sensitive;
    bool history_enabled;
    u32 history_limit;
    bool history_persist;
    bool history_ignore_duplicates;
    bool history_ignore_space;
} shoot_config_t;

void shoot_config_defaults(shoot_config_t *config);
/* Loads the optional current-user config. Missing files use built-in defaults
 * without creating directories or files. Legacy settings are copied
 * non-destructively when possible. */
bool shoot_config_reload(shoot_config_t *config);
/* Resolves the current user's legacy Shoot paths. Kept until history
 * migration is performed by shoot_history. */
bool shoot_config_paths(char *directory, usize directory_capacity,
                        char *config, usize config_capacity,
                        char *history, usize history_capacity);
