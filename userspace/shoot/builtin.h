#pragma once

#include <types.h>

#define SHOOT_MAX_ARGUMENTS 8

typedef struct shell_state {
    char cwd[256];
} shell_state_t;

typedef struct shell_command {
    const char *name;
    const char *arguments[SHOOT_MAX_ARGUMENTS];
    usize argument_count;
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
    shell_builtin_handler_t handler;
} shell_command_info_t;

const shell_command_info_t *find_command(const char *name);
const shell_command_info_t *find_builtin(const char *name);

bool command_arity_is_valid(const char *usage, usize count,
                            usize minimum, usize maximum);

bool execute_builtin(shell_state_t *state, const shell_command_t *command);
