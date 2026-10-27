/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <mg/error.h>
#include <mg/terminal.h>
#include "../common/help.h"

int main(int argc, char **argv)
{
    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 1) {
        command_usage_error(argv[0], "clear", argc > 1 ? argv[1] : NULL);
        return 1;
    }
    mg_terminal_capabilities_t capabilities;
    mg_result_t result = terminal_get_capabilities(&capabilities);
    if (result != MG_OK) {
        fprintf(stderr, "clear: could not query terminal: %s\n",
                error_string(result));
        return 1;
    }
    if (!(capabilities.capabilities & MG_TERMINAL_CAP_INTERACTIVE_OUTPUT))
        return 0;
    result = terminal_clear_screen();
    if (result != MG_OK) {
        fprintf(stderr, "clear: could not clear terminal: %s\n",
                error_string(result));
        return 1;
    }
    return 0;
}
