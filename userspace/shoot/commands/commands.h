#pragma once

#include <types.h>
#include "../builtin.h"

bool execute_clear(shell_state_t *state, const shell_command_t *command);
bool execute_exit(shell_state_t *state, const shell_command_t *command);
bool execute_help(shell_state_t *state, const shell_command_t *command);
bool execute_jump(shell_state_t *state, const shell_command_t *command);
bool execute_list(shell_state_t *state, const shell_command_t *command);
bool execute_locate(shell_state_t *state, const shell_command_t *command);
bool execute_move(shell_state_t *state, const shell_command_t *command);
bool execute_plant(shell_state_t *state, const shell_command_t *command);
bool execute_read(shell_state_t *state, const shell_command_t *command);
bool execute_remove(shell_state_t *state, const shell_command_t *command);
bool execute_version(shell_state_t *state, const shell_command_t *command);
bool execute_where(shell_state_t *state, const shell_command_t *command);
