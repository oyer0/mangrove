/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "../builtin.h"

bool execute_exit(shell_state_t *state, const shell_command_t *command);
bool execute_help(shell_state_t *state, const shell_command_t *command);
bool execute_cd(shell_state_t *state, const shell_command_t *command);
bool execute_reload(shell_state_t *state, const shell_command_t *command);
bool execute_history(shell_state_t *state, const shell_command_t *command);
