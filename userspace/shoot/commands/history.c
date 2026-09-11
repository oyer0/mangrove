/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>
#include "../history.h"
#include "commands.h"

static bool parse_history_count(const char *text, usize *out_count)
{
    usize value = 0;
    usize digits = 0;

    if (!text || !out_count) return false;
    while (*text >= '0' && *text <= '9') {
        usize digit = (usize)(*text++ - '0');

        if (digits >= 4 || value > (4096U - digit) / 10U) return false;
        value = value * 10U + digit;
        digits++;
    }
    if (!digits || *text || value == 0) return false;
    *out_count = value;
    return true;
}

bool execute_history(shell_state_t *state, const shell_command_t *command)
{
    mg_line_history_t *history;
    usize first = 0;
    usize count;

    if (!state || !command || !state->history) {
        printf("history unavailable\n");
        return true;
    }

    history = shoot_history_editor(state->history);
    if (!history) {
        printf("history unavailable\n");
        return true;
    }

    if (command->argument_count == 1 &&
        !strcmp(command->arguments[0], "clear")) {
        shoot_history_clear(state->history);
        if (!state->config.history_enabled)
            printf("history is disabled\n");
        else
            printf("History cleared.\n");
        return true;
    }

    if (!state->config.history_enabled) {
        printf("history is disabled\n");
        return true;
    }

    if (command->argument_count == 1) {
        usize newest;

        if (!parse_history_count(command->arguments[0], &newest)) {
            printf("Usage: history [N|clear]\n");
            return true;
        }
        if (newest < history->count) first = history->count - newest;
    }

    count = history->count;
    for (usize index = first; index < count; index++) {
        const char *entry = line_editor_history_at(history, index);

        if (entry) printf("%u  %s\n", (unsigned)(index + 1U), entry);
    }
    return true;
}
