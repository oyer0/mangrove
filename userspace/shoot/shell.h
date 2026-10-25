/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>
#include "builtin.h"

void shell_run(void);
__attribute__((noreturn)) void shell_exit(shell_state_t *state, i32 status);
bool shell_state_init(shell_state_t *state);
void shell_state_destroy(shell_state_t *state);
bool shell_execute_line(shell_state_t *state, const char *line,
                        const char *source, usize line_number,
                        bool script_mode);
int shell_run_script(const char *path);
