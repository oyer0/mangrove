#include <stdio.h>
#include <string.h>
#include "builtin.h"
#include "commands/commands.h"

static const shell_builtin_t shell_builtins[] = {
    {"clear", "clear", "clear the visible terminal",
     "Clears the visible terminal while preserving the current working directory and shell history.",
     0, 0, execute_clear},
    {"exit", "exit", "leave Shoot",
     "Exits the current Shoot session.",
     0, 0, execute_exit},
    {"help", "help [command]", "show available commands",
     "Shows available commands or detailed help for a specific command.",
     0, 1, execute_help},
    {"jump", "jump <path>", "change the current directory",
     "Changes Shoot's current working directory to the specified path.",
     1, 1, execute_jump},
    {"list", "list [path]", "list directory contents",
     "Lists the current working directory when no path is provided.\nDirectories are displayed with a trailing slash.",
     0, 1, execute_list},
    {"locate", "locate <name>", "show how a command resolves",
     "Shows the full executable path Shoot would use to run a command.",
     1, 1, execute_locate},
    {"move", "move <source> <destination>", "rename or move a file",
     "Renames or moves a file or directory from source to destination.",
     2, 2, execute_move},
    {"plant", "plant <name>", "create an empty file",
     "Creates a new empty file at the specified path.",
     1, 1, execute_plant},
    {"read", "read <file>", "print file contents",
     "Prints the text contents of a file to the terminal.",
     1, 1, execute_read},
    {"remove", "remove <path>", "remove a file or empty directory",
     "Removes a file or an empty directory.\nNon-empty directories cannot be removed.",
     1, 1, execute_remove},
    {"version", "version", "show system version information",
     "Shows the Mangrove and Rhizome kernel versions.",
     0, 0, execute_version},
    {"where", "where", "print the current directory",
     "Prints the full path of the current working directory.",
     0, 0, execute_where},
};

static const shell_external_t shell_externals[] = {
    {"fstest", "fstest", "filesystem API validation",
     "Runs the filesystem API validation test suite.", false},
    {"hello", "hello", "run the Hello validation program",
     "Runs the Hello validation program from /bin.", true},
    {"shoot", "shoot [-v | --version]", "run the Shoot shell",
     "Launches the Shoot command line interface.", true},
};

const shell_builtin_t *get_shell_builtins(usize *out_count)
{
    if (out_count) *out_count = sizeof(shell_builtins) / sizeof(shell_builtins[0]);
    return shell_builtins;
}

const shell_external_t *get_shell_externals(usize *out_count)
{
    if (out_count) *out_count = sizeof(shell_externals) / sizeof(shell_externals[0]);
    return shell_externals;
}

const shell_builtin_t *find_builtin(const char *name)
{
    usize count = sizeof(shell_builtins) / sizeof(shell_builtins[0]);
    for (usize i = 0; i < count; i++) {
        if (strcmp(shell_builtins[i].name, name) == 0) return &shell_builtins[i];
    }
    return NULL;
}

const shell_external_t *find_external(const char *name)
{
    usize count = sizeof(shell_externals) / sizeof(shell_externals[0]);
    for (usize i = 0; i < count; i++) {
        if (strcmp(shell_externals[i].name, name) == 0) return &shell_externals[i];
    }
    return NULL;
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
    const shell_builtin_t *builtin = find_builtin(command->name);

    if (!builtin) return false;
    if (!command_arity_is_valid(builtin->usage,
                                command->argument_count,
                                builtin->minimum_arguments,
                                builtin->maximum_arguments)) return true;
    return builtin->handler(state, command);
}
