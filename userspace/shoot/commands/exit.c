/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/process.h>
#include <stdio.h>
#include <string.h>
#include "commands.h"

static bool parse_status(const char *text, i32 *out_status)
{
    i64 value = 0;
    bool negative = false;
    const char *cursor = text;

    if (!text || !*text || !out_status) return false;
    if (*cursor == '+' || *cursor == '-') {
        negative = *cursor == '-';
        cursor++;
    }
    if (!*cursor) return false;
    while (*cursor) {
        i64 digit;

        if (*cursor < '0' || *cursor > '9') return false;
        digit = (i64)(*cursor - '0');
        if (value > ((i64)2147483647 - digit) / 10) return false;
        value = value * 10 + digit;
        cursor++;
    }
    if (negative) value = -value;
    if (value < -2147483648LL || value > 2147483647LL) return false;
    *out_status = (i32)value;
    return true;
}

bool execute_exit(shell_state_t *state, const shell_command_t *command)
{
    i32 status = 0;

    (void)state;
    if (command && command->argument_count) {
        if (!parse_status(command->arguments[0], &status)) {
            printf("Invalid exit status.\n");
            return true;
        }
    }
    process_exit(status);
}
