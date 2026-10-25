/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mangrove.h>
#include <stdio.h>
#include <stdlib.h>
#include "../common/help.h"
#include "../common/path.h"

int main(int argc, char **argv)
{
    COMMAND_PATH_AUTO char *cwd = NULL;
    mg_result_t result;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 1) {
        command_usage_error(argv[0], "where", argc > 1 ? argv[1] : NULL);
        return 1;
    }
    result = command_getcwd_alloc(&cwd);
    if (result_is_error(result)) {
        printf("Could not read current directory: %s.\n", error_string(result));
        return 1;
    }
    printf("%s\n", cwd);
    return 0;
}
