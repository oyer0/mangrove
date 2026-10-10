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

    if (command->argument_count) {
        const char *option = command->arguments[0];
        mg_result_t result = MG_OK;
        if (command->argument_count == 2 && !strcmp(option, "-d")) {
            usize number;
            if (!parse_history_count(command->arguments[1], &number) ||
                !shoot_history_delete(state->history, number))
                printf("history: invalid entry number\n");
            return true;
        }
        if (command->argument_count != 1) goto usage;
        if (!strcmp(option, "-c")) shoot_history_clear(state->history);
        else if (!strcmp(option, "-a")) result = shoot_history_append(state->history);
        else if (!strcmp(option, "-n")) result = shoot_history_read(state->history, true);
        else if (!strcmp(option, "-r")) result = shoot_history_read(state->history, false);
        else if (!strcmp(option, "-w")) result = shoot_history_write(state->history);
        else goto usage;
        if (result == MG_ERR_RETRY)
            printf("history: saved file changed ambiguously; imported nothing; baseline refreshed. Use history -r to read it.\n");
        else if (result == MG_ERR_UNCERTAIN)
            printf("history: write outcome uncertain; append blocked. Entries remain in memory; a successful history -w resolves this.\n");
        else if (result != MG_OK) printf("history: %s\n", error_string(result));
        return true;
    }

    count = history->count;
    for (usize index = first; index < count; index++) {
        const char *entry = line_editor_history_at(history, index);

        if (entry) printf("%u  %s\n", (unsigned)(index + 1U), entry);
    }
    return true;
usage:
    printf("Usage: history [-c|-a|-n|-r|-w|-d N]\n");
    return true;
}
