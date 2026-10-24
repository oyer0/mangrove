/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>
#include "help.h"

static void print_name_column(const char *name, usize width)
{
    usize length = name ? strlen(name) : 0U;

    printf("%s", name ? name : "");
    while (length++ < width) printf(" ");
}

bool render_help(const shell_command_t *command)
{
    if (!command || command->argument_count != 0U) {
        printf("Usage: help\n");
        return true;
    }

    printf("Shoot built-in commands:\n\n");
    for (usize index = 0; index < shell_builtin_count(); index++) {
        const shell_command_info_t *builtin = shell_builtin_at(index);
        if (!builtin) continue;
        printf("  ");
        print_name_column(builtin->name, 10U);
        printf("%s\n", builtin->description);
    }
    return true;
}
