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

typedef struct shell_builtin {
    const char *name;
    const char *usage;
    const char *description;
    const char *help;
    usize minimum_arguments;
    usize maximum_arguments;
    shell_builtin_handler_t handler;
} shell_builtin_t;

typedef struct shell_external {
    const char *name;
    const char *usage;
    const char *description;
    const char *help;
    bool visible_in_help;
} shell_external_t;

const shell_builtin_t *get_shell_builtins(usize *out_count);
const shell_external_t *get_shell_externals(usize *out_count);

const shell_builtin_t *find_builtin(const char *name);
const shell_external_t *find_external(const char *name);

bool command_arity_is_valid(const char *usage, usize count,
                            usize minimum, usize maximum);

bool execute_builtin(shell_state_t *state, const shell_command_t *command);
