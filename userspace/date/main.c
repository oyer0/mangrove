/* SPDX-License-Identifier: GPL-3.0-only */
#include <mangrove.h>
#include <stdio.h>
#include "../common/help.h"

int main(int argc, char **argv)
{
    mg_mangrove_time_t now;
    mg_calendar_time_t calendar;
    mg_result_t result;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 1) {
        command_usage_error(argv[0], "date", argc > 1 ? argv[1] : NULL);
        return 1;
    }

    result = mg_clock_realtime(&now);
    if (result != MG_OK ||
        mangrove_time_to_calendar(&now, &calendar) != MG_OK) {
        printf("System time is unavailable.\n");
        return 1;
    }
    printf("%04d-%02u-%02u %02u:%02u:%02u UTC\n",
           calendar.year, calendar.month, calendar.day,
           calendar.hour, calendar.minute, calendar.second);
    return 0;
}
