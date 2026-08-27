#include <stdio.h>
#include "help.h"

bool render_help(const shell_command_t *command)
{
    const shell_command_info_t *topic;

    if (!command || command->argument_count == 0) {
        printf("Usage:\n  help <command>\n");
        return true;
    }

    topic = find_command(command->arguments[0]);
    if (topic) {
        printf("%s\n\n%s\n", topic->usage, topic->help);
        return true;
    }

    printf("Unknown help topic: %s\n", command->arguments[0]);
    return true;
}
