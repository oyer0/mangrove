/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <mg/terminal.h>

typedef struct shoot_config {
    mg_terminal_color_t prompt_color;
    mg_terminal_color_t completion_color;
    bool completion_case_sensitive;
} shoot_config_t;

void shoot_config_defaults(shoot_config_t *config);
/* Loads the current user's config.  A missing config uses defaults and is
 * created when the user's home directory permits it.  On a malformed or
 * unreadable existing config, the caller's previous settings are unchanged. */
bool shoot_config_reload(shoot_config_t *config);
