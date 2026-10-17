/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mg/process.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "commands.h"
#include "../shell.h"

static bool parse_status(const char *text, i32 *out_status)
{
    const char *digits;
    char *end;
    int saved_errno;
    long value;

    if (!text || !*text || !out_status) return false;
    digits = text;
    if (*digits == '+' || *digits == '-') digits++;
    if (!*digits) return false;
    for (const char *cursor = digits; *cursor; cursor++)
        if (*cursor < '0' || *cursor > '9') return false;

    /* Keep Shoot's strict signed-decimal syntax while using loam's checked
     * conversion for the full i32 range, including INT32_MIN. */
    saved_errno = errno;
    errno = 0;
    value = strtol(text, &end, 10);
    bool range_error = errno == ERANGE;
    errno = saved_errno;
    if (range_error || end == text || *end ||
        value < (long)INT32_MIN || value > (long)INT32_MAX) return false;
    *out_status = (i32)value;
    return true;
}

bool execute_exit(shell_state_t *state, const shell_command_t *command)
{
    i32 status = 0;

    if (command && command->argument_count) {
        if (!parse_status(command->arguments[0], &status)) {
            printf("Invalid exit status.\n");
            return true;
        }
    }
    shell_exit(state, status);
}
