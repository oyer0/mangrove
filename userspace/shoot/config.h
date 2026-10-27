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
bool shoot_config_parse(const char *text, usize length,
                        shoot_config_t *config, char *diagnostic,
                        usize diagnostic_capacity);
/* Loads the optional current-user config. Missing files use built-in defaults
 * without creating directories or files. Invalid or unreadable files leave
 * built-in defaults active. Valid legacy settings are copied non-destructively
 * when possible. */
bool shoot_config_reload(shoot_config_t *config, char *diagnostic,
                         usize diagnostic_capacity);
