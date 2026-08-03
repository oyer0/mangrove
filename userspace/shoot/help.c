#include <stdio.h>
#include <string.h>
#include "help.h"

void append_padded(char *buf, usize *pos, usize max, const char *str, usize width)
{
    usize len = strlen(str);
    if (*pos >= max - 1) return;
    *pos += (usize)snprintf(buf + *pos, max - *pos, "%s", str);
    for (usize i = len; i < width; i++) {
        if (*pos < max - 1) {
            buf[*pos] = ' ';
            (*pos)++;
            buf[*pos] = '\0';
        }
    }
}

bool render_help(const shell_command_t *command)
{
    const shell_builtin_t *topic;
    usize builtin_count = 0;
    usize external_count = 0;
    const shell_builtin_t *builtins = get_shell_builtins(&builtin_count);
    const shell_external_t *externals = get_shell_externals(&external_count);

    if (!command || command->argument_count == 0) {
        char help_buf[2048];
        usize pos = 0;
        pos += (usize)snprintf(help_buf + pos, sizeof(help_buf) - pos, "Available commands:\n");
        for (usize i = 0; i < builtin_count; i++) {
            pos += (usize)snprintf(help_buf + pos, sizeof(help_buf) - pos, "  ");
            append_padded(help_buf, &pos, sizeof(help_buf), builtins[i].name, 9);
            pos += (usize)snprintf(help_buf + pos, sizeof(help_buf) - pos, "%s\n",
                                  builtins[i].description);
        }
        for (usize i = 0; i < external_count; i++) {
            if (!externals[i].visible_in_help) continue;
            pos += (usize)snprintf(help_buf + pos, sizeof(help_buf) - pos, "  ");
            append_padded(help_buf, &pos, sizeof(help_buf), externals[i].name, 9);
            pos += (usize)snprintf(help_buf + pos, sizeof(help_buf) - pos, "%s\n",
                                  externals[i].description);
        }
        printf("%s", help_buf);
        return true;
    }

    topic = find_builtin(command->arguments[0]);
    if (topic) {
        printf("%s - %s\n\nUsage:\n    %s\n\n%s\n",
               topic->name, topic->description, topic->usage, topic->help);
        return true;
    }
    const shell_external_t *external = find_external(command->arguments[0]);
    if (external) {
        printf("%s - %s\n\nUsage:\n    %s\n\n%s\n",
               external->name, external->description, external->usage, external->help);
        return true;
    }

    printf("Unknown help topic: %s\n", command->arguments[0]);
    return true;
}
