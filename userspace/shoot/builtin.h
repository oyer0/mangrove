/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <types.h>
#include "config.h"

struct shoot_history;

#define SHOOT_MAX_ARGUMENTS 8
#define SHOOT_COMMAND_STORAGE_CAPACITY 512
#define SHOOT_LINE_CAPACITY 512

typedef enum shell_completion_kind {
    SHELL_COMPLETION_NONE,
    SHELL_COMPLETION_PATH,
    SHELL_COMPLETION_DIRECTORY,
    SHELL_COMPLETION_COMMAND,
} shell_completion_kind_t;

typedef struct shell_state {
    char cwd[256];
    shoot_config_t config;
    struct shoot_history *history;
    char completion_line[SHOOT_LINE_CAPACITY];
    usize completion_cursor;
    bool completion_pending;
} shell_state_t;

typedef struct shell_command {
    const char *name;
    const char *arguments[SHOOT_MAX_ARGUMENTS];
    bool argument_home_expand[SHOOT_MAX_ARGUMENTS];
    usize argument_count;
    const char *output_path;
    bool output_home_expand;
    bool output_append;
    char storage[SHOOT_COMMAND_STORAGE_CAPACITY];
} shell_command_t;

typedef bool (*shell_builtin_handler_t)(shell_state_t *state,
                                        const shell_command_t *command);

typedef struct shell_command_info {
    const char *name;
    const char *usage;
    const char *description;
    const char *help;
    usize minimum_arguments;
    usize maximum_arguments;
    shell_completion_kind_t completion_kind;
    shell_builtin_handler_t handler;
} shell_command_info_t;

const shell_command_info_t *find_command(const char *name);
const shell_command_info_t *find_builtin(const char *name);
usize shell_builtin_count(void);
const shell_command_info_t *shell_builtin_at(usize index);

bool command_arity_is_valid(const char *usage, usize count,
                            usize minimum, usize maximum);

bool execute_builtin(shell_state_t *state, const shell_command_t *command);
