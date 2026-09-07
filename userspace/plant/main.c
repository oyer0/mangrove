/* SPDX-License-Identifier: GPL-3.0-only */
#include <mangrove.h>
#include <stdio.h>
#include "../common/help.h"
#include "../common/path.h"

int main(int argc, char **argv)
{
    char path[256];
    mg_result_t result;

    if (command_help_requested(argc, argv))
        return command_print_help(argv[0]);
    if (argc != 2) {
        command_usage_error(argv[0], "plant <name>",
                            argc > 1 && argv[1][0] == '-' ? argv[1] : NULL);
        return 1;
    }
    if (!command_resolve_path(argv[1], path, sizeof(path))) {
        printf("Could not create \"%s\": invalid path.\n", argv[1]);
        return 1;
    }
    result = file_create(path);
    if (result == MG_ERR_ALREADY_EXISTS) {
        printf("Could not create \"%s\": already exists.\n", argv[1]);
        return 1;
    }
    if (result_is_error(result)) {
        printf("Could not create \"%s\": %s.\n", argv[1], error_string(result));
        return 1;
    }
    return 0;
}
