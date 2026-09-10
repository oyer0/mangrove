/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>
#include "builtin.h"
#include "commands/commands.h"

/* Only commands executed inside Shoot belong in this table.  External
 * command discovery comes from /share/help and /bin at runtime. */
static const shell_command_info_t shell_builtins[] = {
    {"cd", "cd [path]", "change Shoot's current directory",
     "Changes Shoot's current working directory. With no path, changes to the current user's home directory.",
     0, 1, SHELL_COMPLETION_DIRECTORY, execute_cd},
    {"help", "help [category|command]", "show command documentation",
     "Shows command categories or documentation for one command.", 0, 2,
     SHELL_COMPLETION_COMMAND, execute_help},
    {"reload", "reload", "reload Shoot's configuration",
     "Reloads ~/.shoot/config without restarting Shoot.", 0, 0,
     SHELL_COMPLETION_NONE, execute_reload},
    {"exit", "exit", "leave Shoot", "Exits the current Shoot session.",
     0, 0, SHELL_COMPLETION_NONE, execute_exit},
};

usize shell_builtin_count(void)
{
    return sizeof(shell_builtins) / sizeof(shell_builtins[0]);
}

const shell_command_info_t *shell_builtin_at(usize index)
{
    return index < shell_builtin_count() ? &shell_builtins[index] : NULL;
}

const shell_command_info_t *find_command(const char *name)
{
    for (usize index = 0; index < shell_builtin_count(); index++) {
        if (!strcmp(shell_builtins[index].name, name))
            return &shell_builtins[index];
    }
    return NULL;
}

const shell_command_info_t *find_builtin(const char *name)
{
    return find_command(name);
}

bool command_arity_is_valid(const char *usage, usize count,
                            usize minimum, usize maximum)
{
    if (count >= minimum && count <= maximum) return true;
    printf("Usage: %s\n", usage);
    return false;
}

bool execute_builtin(shell_state_t *state, const shell_command_t *command)
{
    const shell_command_info_t *builtin = find_builtin(command->name);

    if (!builtin) return false;
    if (!command_arity_is_valid(builtin->usage,
                                command->argument_count,
                                builtin->minimum_arguments,
                                builtin->maximum_arguments)) return true;
    return builtin->handler(state, command);
}
