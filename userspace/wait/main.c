/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include "../common/help.h"

#define WAIT_MILLISECONDS_PER_SECOND 1000ULL

static bool parse_seconds(const char *text, u64 *seconds)
{
    u64 value = 0;

    if (!text || !*text || !seconds) return false;
    for (const char *cursor = text; *cursor; cursor++) {
        u64 digit;

        if (*cursor < '0' || *cursor > '9') return false;
        digit = (u64)(*cursor - '0');
        if (value > (~(u64)0 - digit) / 10ULL) return false;
        value = value * 10ULL + digit;
    }
    if (value > ~(u64)0 / WAIT_MILLISECONDS_PER_SECOND) return false;
    *seconds = value;
    return true;
}

int main(int argc, char **argv)
{
    u64 seconds;
    mg_result_t result;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 2 || !parse_seconds(argv[1], &seconds)) {
        command_usage_error(argv[0], "wait SECONDS",
                            argc > 1 ? argv[1] : NULL);
        return 1;
    }
    result = process_sleep_ms(seconds * WAIT_MILLISECONDS_PER_SECOND);
    if (result_is_error(result)) {
        printf("Could not wait: %s.\n", error_string(result));
        return 1;
    }
    return 0;
}
