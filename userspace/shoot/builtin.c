#include <stdio.h>
#include <string.h>
#include "builtin.h"
#include "commands/commands.h"

static const shell_command_info_t shell_commands[] = {
    {"jump", "jump <path>", "change Shoot's current directory",
     "Changes Shoot's current working directory.",
     SHELL_COMMAND_BUILTIN, 1, 1, execute_jump},
    {"help", "help [command]", "show command documentation",
     "Shows Shoot help or documentation for one command.",
     SHELL_COMMAND_BUILTIN, 0, 1, execute_help},
    {"exit", "exit", "leave Shoot",
     "Exits the current Shoot session.",
     SHELL_COMMAND_BUILTIN, 0, 0, execute_exit},

    {"clear", "clear", "clear the visible terminal",
     "Clears the visible terminal.", SHELL_COMMAND_EXTERNAL, 0, 0, NULL},
    {"copy", "copy <source> <destination>", "copy a file",
     "Copies a regular file without changing the source.",
     SHELL_COMMAND_EXTERNAL, 2, 2, NULL},
    {"fetch", "fetch <url>", "download a file",
     "Downloads an HTTP file to the current directory. Existing files are not overwritten.",
     SHELL_COMMAND_EXTERNAL, 1, 1, NULL},
    {"list", "list [path]", "list directory contents",
     "Lists a directory. Directories are displayed with a trailing slash.",
     SHELL_COMMAND_EXTERNAL, 0, 1, NULL},
    {"locate", "locate <name>", "show how a command resolves",
     "Shows whether a name is a Shoot builtin or its /bin executable path.",
     SHELL_COMMAND_EXTERNAL, 1, 1, NULL},
    {"move", "move <source> <destination>", "rename or move a file",
     "Renames or moves a file or directory.",
     SHELL_COMMAND_EXTERNAL, 2, 2, NULL},
    {"network", "network [interfaces|routes|neighbors|connections|dns|renew|automatic|manual|reload]",
     "inspect network state",
     "Shows or changes network configuration, interfaces, routes, neighbors, connections, or DNS. Changes made by manual and automatic are session-only; reload reads /core/network/config.",
     SHELL_COMMAND_EXTERNAL, 0, 4, NULL},
    {"power", "power", "show battery and AC status",
     "Shows current battery and AC adapter status.",
     SHELL_COMMAND_EXTERNAL, 0, 0, NULL},
    {"ping", "ping <host> | ping [-c|--count] <count> <host>",
     "send ICMP echo requests",
     "Sends ICMP echo requests. Counts range from 1 to 16; the default is 4.",
     SHELL_COMMAND_EXTERNAL, 1, 3, NULL},
    {"plant", "plant <name>", "create an empty file",
     "Creates a new empty file and refuses existing files.",
     SHELL_COMMAND_EXTERNAL, 1, 1, NULL},
    {"read", "read <file>", "print file contents",
     "Prints the text contents of a local file.",
     SHELL_COMMAND_EXTERNAL, 1, 1, NULL},
    {"remove", "remove <path>", "remove a file or empty directory",
     "Removes a file or an empty directory.",
     SHELL_COMMAND_EXTERNAL, 1, 1, NULL},
    {"resolve", "resolve <hostname>", "resolve an IPv4 address",
     "Resolves a hostname to an IPv4 address.",
     SHELL_COMMAND_EXTERNAL, 1, 1, NULL},
    {"say", "say [text ...]", "print text",
     "Prints its arguments separated by spaces and ending with a newline.",
     SHELL_COMMAND_EXTERNAL, 0, SHOOT_MAX_ARGUMENTS, NULL},
    {"shutdown", "shutdown", "power off the system",
     "Powers off Mangrove immediately.", SHELL_COMMAND_EXTERNAL, 0, 0, NULL},
    {"reboot", "reboot", "restart the system",
     "Restarts Mangrove immediately.", SHELL_COMMAND_EXTERNAL, 0, 0, NULL},
    {"uptime", "uptime", "show elapsed system uptime",
     "Shows elapsed time since Mangrove booted.",
     SHELL_COMMAND_EXTERNAL, 0, 0, NULL},
    {"version", "version", "show system version information",
     "Shows Mangrove and Pith system version information.",
     SHELL_COMMAND_EXTERNAL, 0, 0, NULL},
    {"where", "where", "print the current directory",
     "Prints the current working directory inherited from Shoot.",
     SHELL_COMMAND_EXTERNAL, 0, 0, NULL},
};

static usize shell_command_count(void)
{
    return sizeof(shell_commands) / sizeof(shell_commands[0]);
}

const shell_command_info_t *find_command(const char *name)
{
    for (usize i = 0; i < shell_command_count(); i++) {
        if (strcmp(shell_commands[i].name, name) == 0) return &shell_commands[i];
    }
    return NULL;
}

const shell_command_info_t *find_builtin(const char *name)
{
    const shell_command_info_t *command = find_command(name);
    return command && command->kind == SHELL_COMMAND_BUILTIN ? command : NULL;
}

const shell_command_info_t *find_external(const char *name)
{
    const shell_command_info_t *command = find_command(name);
    return command && command->kind == SHELL_COMMAND_EXTERNAL ? command : NULL;
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
