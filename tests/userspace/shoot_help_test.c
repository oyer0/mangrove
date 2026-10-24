/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../../userspace/shoot/builtin.h"

static char output[2048];
static usize output_length;
static const shell_command_info_t commands[] = {
    {"jump", "jump [path]", "change directory", "", 0, 1,
     SHELL_COMPLETION_DIRECTORY, NULL},
    {"help", "help", "list built-in commands", "", 0, 0,
     SHELL_COMPLETION_COMMAND, NULL},
    {"history", "history", "manage history", "", 0, 0,
     SHELL_COMPLETION_NONE, NULL},
};

static int capture_printf(const char *format, ...)
{
    va_list arguments;
    int written;

    if (output_length >= sizeof(output)) return -1;
    va_start(arguments, format);
    written = vsnprintf(output + output_length,
                        sizeof(output) - output_length, format, arguments);
    va_end(arguments);
    if (written < 0 || (usize)written >= sizeof(output) - output_length)
        return -1;
    output_length += (usize)written;
    return written;
}

#define printf capture_printf
#include "../../userspace/shoot/help.c"
#undef printf

usize shell_builtin_count(void)
{
    return sizeof(commands) / sizeof(commands[0]);
}

const shell_command_info_t *shell_builtin_at(usize index)
{
    return index < shell_builtin_count() ? &commands[index] : NULL;
}

const shell_command_info_t *find_builtin(const char *name)
{
    (void)name;
    assert(!"help must not resolve arbitrary command records");
    return NULL;
}

static void reset(void)
{
    memset(output, 0, sizeof(output));
    output_length = 0U;
}

int main(void)
{
    shell_command_t command = {0};

    reset();
    assert(render_help(&command));
    assert(strstr(output, "Shoot built-in commands:") != NULL);
    assert(strstr(output, "jump") != NULL);
    assert(strstr(output, "help") != NULL);
    assert(strstr(output, "history") != NULL);
    assert(strstr(output, "change directory") != NULL);
    assert(strstr(output, "/bin") == NULL);

    reset();
    command.argument_count = 1U;
    command.arguments[0] = "external-command";
    assert(render_help(&command));
    assert(!strcmp(output, "Usage: help\n"));
    return 0;
}
